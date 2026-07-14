#pragma once
#include <atomic>
#include <bit>
#include <vector>
#include <cstdlib>
#include <thread>
#include <__thread/this_thread.h>

template <class K, class V>
class FixedSizeHashMap;
template <typename T>
class ArenaPart {
    // The reason _size is private with a getter instead of public is because I'd rather have the constructor initialise
    // it first but I want members before methods and don't want to intermix public and private multiple times. So
    // for that reason, all the members are private
    const size_t _size;
    std::atomic<size_t> index{};
    T* data;
public:
    size_t size() const {
        return _size;
    }
    ArenaPart(size_t size) : _size(size), data(static_cast<T *>(::operator new(sizeof(T) * size, std::align_val_t{alignof(T)}))) {
        std::memset(static_cast<void *>(data), 0, sizeof(T)*size);
        // I'm zeroing so that calling the destructor when memory is unitialised isn't as bad. I'll just leave it
        // to the caller to initialise memory further.
    }
    ~ArenaPart() {
        for (size_t i = 0; i < index; i++) {
            data[i].~T();
        }
        ::operator delete (data, std::align_val_t{alignof(T)});
    }
    T* getMemory() {
        auto i = index.fetch_add(1, std::memory_order_relaxed);
        return data + i;
    }
    bool full() {
        return index.load(std::memory_order_relaxed) == size() - 1;
    }
    ArenaPart(const ArenaPart&) = delete;
};

template <typename T, size_t size>
class FillableArray {
    // I wanted to create a stack array with a specific value without needed a default constructor or listing it out
    // many times.
    // When writing this codebase, in many places, when faced with C++ lacking a neat way to do a specific thing,
    // I just did the specific thing at the cost of messy code. Instead of settling.
    using ArrayType = std::array<T, size>;
    alignas(alignof(ArrayType)) char data[sizeof(ArrayType)];
public:
    std::array<T, size>& asArray() {
        return *reinterpret_cast<ArrayType*>(&data);
    }
    template <typename... Args>
    FillableArray(Args&&... args) {
        for (auto& ele : asArray()) {
            new(&ele) T(std::forward<Args>(args)...);
        }
        // I'm not concerned with memory leaks if a constructor throws here
    }
    ~FillableArray() {
        asArray().~ArrayType();
    }

};
template <typename T>
class Arena {
    static constexpr size_t N = 32;
    FillableArray<ArenaPart<T>, N> parts;
public:
    Arena(size_t size) : parts((size + N - 1) / N) {}
    T* getMemory() {
        auto hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (auto i = hash & (N - 1); ; i = (i + 1) & (N - 1))  {
            if (!parts.asArray()[i].full()) {
                return parts.asArray()[i].getMemory();
            }
        }
    }
};
template <class K, class V>
class alignas(64) Payload {
    public:
    const K key;
    const V value;
    private:
    const u_int64_t hash;
    template <class KArg, class VArg, class u64Arg>
    // Instead of thinking about what the signature should be, I just decided to cover all my bases.
    // I wanted the empty constructor gone, so I needed to do it this way instead of relying on the default aggregate
    // initialisation.
    Payload(KArg&& key, VArg&& value, u64Arg&& hash) :
        key(std::forward<KArg>(key)),
        value(std::forward<VArg>(value)),
        hash(std::forward<u64Arg>(hash)) {}
    friend class FixedSizeHashMap<K, V>;
};

template<class Payload>
class alignas(64) PackedPointer {
    // The pointer part of this should be treated as const.
    std::uintptr_t value;
    public:
    PackedPointer(Payload* value = nullptr) : value(reinterpret_cast<std::uintptr_t>(value)) {}
    static PackedPointer makeDead(Payload* value) {
        return {reinterpret_cast<std::uintptr_t>(value) | 1};
    }
    Payload* ptr() const {
        return reinterpret_cast<Payload *>(value & -2);
    }
    auto operator->() const {
        return ptr();
    }
    auto operator*() const {
        return ptr();
    }
    [[nodiscard]] bool tombstone() const {
        return value & 1;
    }
    [[nodiscard]] bool empty() const {
        return value == 0;
    }
    void kill() {
        value |= 1;
    }
    operator bool() const {
        return value;
    }
};

template <class K, class V>
class FixedSizeHashMap {
public:
    using Ptr = PackedPointer<Payload<K, V>>;
private:
    const size_t size;
    Arena<Payload<K,V>> arena;
    std::atomic<Ptr>* const array;
    [[nodiscard]] size_t nextIndex(size_t i) const {
        return (i + 1) & (size - 1);
    }
public:
    FixedSizeHashMap(size_t minSize) : size(1 << (std::bit_width(minSize * 3 / 2)-1)), arena(size), array(new std::atomic<Ptr>[size]) {}
    ~FixedSizeHashMap() {
        delete[] array;
    }
    Ptr get(const K& key) const {
        auto hash = std::hash<K>{}(key);
        auto initial = hash & (size - 1);
        for (auto i = initial; ; i = nextIndex(i)) {
            auto ptr = array[i].load(std::memory_order_relaxed);
            if (ptr.empty()) {
                return ptr;
            }
            if (not ptr.tombstone()) {
                if (ptr->key == key) {
                    return ptr;
                }
            }
            if (nextIndex(i) == initial) {
                return {nullptr};
            }
        }
    }
    bool set(const K& key, const V& value) {
        auto hash = std::hash<K>{}(key);
        auto initial = hash & (size - 1);
        Ptr ptr = {arena.getMemory()};
        ptr = new(ptr.ptr()) Payload<K,V>(key, value, hash);
        auto payload = PackedPointer(ptr);
        for (auto i = initial; ; i = nextIndex(i)) {
            auto ptr = array[i].load(std::memory_order_relaxed);
            if (ptr.empty()) {
                if (array[i].compare_exchange_strong(ptr, payload)) {
                    return true;
                }
            }
            if (not ptr.tombstone()) {
                if (ptr->key == key) {
                    if (array[i].compare_exchange_strong(ptr, payload)) {
                        return true;
                    }
                }
            }
            if (nextIndex(i) == initial) {
                return false;;
            }
        }
    }
    void remove(const K& key) {
        auto hash = std::hash<K>{}(key);
        auto initial = hash & (size - 1);
        for (auto i = initial; ; i = nextIndex(i)) {
            auto ptr = array[i].load(std::memory_order_relaxed);
            if (ptr.empty()) {
                return;
            }
            if (not ptr.tombstone()) {
                if (ptr->key == key) {
                    ptr.kill();
                    array[i].store(ptr, std::memory_order_relaxed);
                }
            }
            if (nextIndex(i) == initial) {
                return;
            }
        }
    }
};

// I really like both high performance and object-oriented programming. While OOP is often bad for performance,
// encapsulation is generally very fine and so, I still use it heavily. As well as templates for polymorphism.