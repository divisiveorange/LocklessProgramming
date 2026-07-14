#pragma once
#include <atomic>
#include <bit>
#include <vector>
#include <cstdlib>

template <class K, class V>
class FixedSizeHashMap;
template <typename T>
class Arena {
    // The reason _size is private with a getter instead of public is because I'd rather have the constructor initialise
    // it first but I want members before methods and don't want to intermix public and private multiple times. So
    // for that reason, all the members are private
    const size_t _size;
    std::atomic<size_t> index{};
    T* data;
public:
    size_t size() {
        return _size;
    }
    Arena(size_t size) : _size(size), data(::operator new(sizeof(T)*size, std::align_val_t{alignof(T)})) {
        std::memset(data, 0, sizeof(T)*size);
        // I'm zeroing so that calling the destructor when memory is unitialised isn't as bad. I'll just leave it
        // to the caller to initialise memory further.
    }
    ~Arena() {
        for (size_t i = 0; i < index; i++) {
            data[i].~T();
        }
        ::operator delete (data, std::align_val_t{alignof(T)});
    }
    T* getMemory() {
        auto i = index.fetch_add(1, std::memory_order_relaxed);
        return data + i;
    }
    Arena(const Arena&) = delete;
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
    Payload* value;
    public:
    PackedPointer(Payload* value = nullptr) : value(value) {}
    static PackedPointer makeDead(Payload* value) {
        return {value | 1};
    }
    Payload* ptr() const {
        return value & ~0x1;
    }
    [[nodiscard]] bool tombstone() const {
        return value & 1;
    }
    [[nodiscard]] bool empty() const {
        return value == nullptr;
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
    const Arena<Payload<K,V>> arena;
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
        auto hash = std::hash(key);
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
        auto hash = std::hash(key);
        auto initial = hash & (size - 1);
        auto payload = PackedPointer(Payload(key, value, hash));
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
        auto hash = std::hash(key);
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