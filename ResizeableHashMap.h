#pragma once
#include <atomic>
#include <stdexcept>
#include <algorithm>
#include <thread>
#include <__new/interference_size.h>
#include <__thread/this_thread.h>
template<typename K, typename V>
class ResizeableHashMap;

template<class K, class V>
class Payload {
public:
    const K key;
    const V value;

private:
    const u_int64_t hash;

    template<class KArg, class VArg, class u64Arg>
    // Instead of thinking about what the signature should be, I just decided to cover all my bases.
    // I wanted the empty constructor gone, so I needed to do it this way instead of relying on the default aggregate
    // initialisation.
    Payload(KArg &&key, VArg &&value, u64Arg &&hash) : key(std::forward<KArg>(key)),
                                                       value(std::forward<VArg>(value)),
                                                       hash(std::forward<u64Arg>(hash)) {
    }

    friend class ResizeableHashMap<K, V>;
};

template<class Payload>
class PackedPointer {
    // The pointer part of this should be treated as const.
    std::uintptr_t value;

public:
    PackedPointer(Payload *value = nullptr) : value(reinterpret_cast<std::uintptr_t>(value)) {
    }
    PackedPointer(std::uintptr_t value) : value(value) {}

    static PackedPointer makeDead(Payload *value) {
        return {reinterpret_cast<std::uintptr_t>(value) | 1};
    }
    PackedPointer makeDead() {
        return makeDead(ptr());
    }

    Payload *ptr() const {
        return reinterpret_cast<Payload *>(value & (~3));
    }

    auto operator->() const {
        return ptr();
    }

    auto operator*() const {
        return *ptr();
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

    bool beingMoved() {
        return value & 2;
    }

    bool alive() {
        return !(value & 3);
    }

    void markMoved() {
        value |= 2;
    }

    operator bool() const {
        return value;
    }
};


template<typename K, typename V>
class ResizeableHashMap {
    using Ptr = PackedPointer<Payload<K, V> >;

    class Maps {
    public:
        class Map {
        public:
            std::atomic<Ptr>* data;
            const size_t size;
            auto& operator[](size_t i) {
                return data[i];
            }
            class Population {
                struct alignas(std::hardware_destructive_interference_size) perThreadCounter : std::atomic<long long> {
                    using std::atomic<long long>::atomic;
                };
                static constexpr size_t incrementSize = 16;
                static constexpr size_t threadHashSize = 32;

                std::atomic<size_t> overEstimate{};
                std::array<perThreadCounter, 32> counters{};

                std::array<perThreadCounter, 32> deletions{};
                ResizeableHashMap& fullMap;
            public:
                Population(ResizeableHashMap& fullMap) : fullMap(fullMap) {}
                size_t getSloppySize() const {
                    return overEstimate.load(std::memory_order_acquire);
                }
                size_t calculateDeletionCount() {
                    size_t count{};
                    for (auto& perHashCount : deletions) {
                        count += perHashCount.load(std::memory_order_acquire);
                    }
                    return count;
                }
                void increment() {
                    auto index = std::hash<std::thread::id>{}(std::this_thread::get_id()) & (threadHashSize-1);
                    auto remaining = counters[index].fetch_sub(1, std::memory_order_acq_rel);
                    if (remaining <= 0) {
                        auto totalCount = overEstimate.fetch_add(incrementSize, std::memory_order_acq_rel);
                        if (totalCount + incrementSize >= fullMap.maps.load(std::memory_order_relaxed)->curr.size / 2) {
                            fullMap.resize(totalCount + incrementSize);
                        }
                    }
                }
                void decrement() {
                    auto index = std::hash<std::thread::id>{}(std::this_thread::get_id()) & (threadHashSize-1);
                    deletions[index].fetch_add(1, std::memory_order_release);
                }
            } population{};
            void moveInsert(PackedPointer<Payload<K,V>> ptr) {
                auto initial = ptr->hash & (size - 1);
                for (auto i = initial; ; i = (i+1) & (size - 1)) {
                    auto slot = data[i].load(std::memory_order_acquire);
                    if (slot.empty()) {
                        population.increment();
                        if (data[i].compare_exchange_strong(slot, {ptr.ptr()})) {
                            return;
                        }
                    }
                    if (slot->hash == ptr->hash && slot->key == ptr->hash) {
                        return;
                    }
                    if (initial == ((i+1) & (size -1 ))) {
                        throw std::runtime_error("Out of space in HashMap");
                    }
                }
            }
            Map(size_t size, ResizeableHashMap& fullMap) : size(size), population(fullMap) {
                data = new std::atomic<Ptr>[size];
            }
        };
        Map& curr,& old;
        std::atomic<size_t> movingIndex{};
        std::atomic<bool>* movedChunks;
        Maps(Map* curr, Map* old) : curr(*curr), old(*old) {
            movedChunks = new std::atomic<bool>[this->old.size / 16]{};
        }
        Maps(size_t oldSize, size_t newSize, ResizeableHashMap& map) : curr(*new Map(newSize, map)), old(*new Map(oldSize, map)) {
        }
        void moveItems(size_t index) {
            if (index < old.size) {
                auto begin = &old.data[index];
                auto end = &old.data[std::max(index + 16, old.size)];
                for (auto it = begin; begin != end; ++begin) {
                    auto ptr = it->load(std::memory_order_acquire);
                    if (ptr.alive()) {
                        auto movedPtr = ptr; movedPtr.markMoved();
                        if (it->compare_exchange_strong(ptr, movedPtr)) {
                            curr.moveInsert(ptr);
                        }
                    }
                }
                movedChunks[index / 16].store(true, std::memory_order_release);
            }
        }
        void ensureAllMoved() {
            for (size_t i = 0; i < old.size; i += 16) {
                if (not movedChunks[i].load(std::memory_order_acquire)) {
                    moveItems(i);
                }
            }
        }
    };

private:
    std::atomic<Maps *> maps;

public:
    ResizeableHashMap() : maps(new Maps(16, 128, *this)) {
    }
    Payload<K, V>* get(const K &key) {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto hash = std::hash<K>{}(key);
        {
            auto& currArr = currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    break;
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        return ptr.ptr();
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    break;
                }
            }
        }
        PackedPointer<Payload<K,V>> targetPtr{};
        {
            auto& currArr = currMaps->old;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    return nullptr;
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        return ptr.ptr();
                    }
                }
                if (ptr.beingMoved()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        targetPtr = ptr.ptr();
                        break;
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return nullptr;
                }
            }
        }
        {
            auto& currArr = currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    return targetPtr.ptr();
                }
                if (ptr.ptr() == targetPtr.ptr()) {
                    if (ptr.alive()) {
                        return ptr.ptr();
                    }
                    return nullptr;
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return targetPtr.ptr();
                }
            }
        }
    }
    bool remove(const K &key) {
       return remove(key, std::hash<K>{}(key));
    }
    bool remove(const K &key, const size_t hash) {
        auto currMaps = maps.load(std::memory_order_acquire);
        {
            auto& currArr = currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    break;
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        auto deadPtr = ptr.makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                return remove(key, hash);
                            }
                            currMaps->curr.population.decrement();
                            return true;
                        }
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    break;
                }
            }
        }
        PackedPointer<Payload<K,V>> targetPtr{};
        {
            auto& currArr = currMaps->old;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    return false;
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        auto deadPtr = ptr.makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                return remove(key, hash);
                            }
                            return true;
                        }
                    }
                }
                if (ptr.beingMoved()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        targetPtr = ptr.ptr();
                        break;
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return false;
                }
            }
        }
        {
            auto& currArr = currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    auto deadPtr = targetPtr.makeDead();
                    currMaps->curr.population.increment();
                    moveItems();
                    if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                        if (!checkCorrectMap(currMaps)) {
                            return remove(key, hash);
                        }
                        currMaps->curr.population.decrement();
                        return true;
                    }
                }
                if (ptr.ptr() == targetPtr.ptr()) {
                    if (ptr.alive()) {
                        auto deadPtr = targetPtr.makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                return remove(key, hash);
                            }
                            currMaps->curr.population.decrement();
                            return true;
                        }
                        return false;

                    }
                    return false;
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    throw std::runtime_error("Out of space in HashMap");
                }
            }
        }
    }
    void insert(const K &key, const V &value) {
        auto hash = std::hash<K>{}(key);
        Ptr ptrToInsert(new Payload<K, V>(key, value, hash));
        insert(key, value, hash, ptrToInsert);
    }

    void insert(const K &key, const V &value, size_t hash, Ptr ptrToInsert) {
        auto currMaps = maps.load(std::memory_order_acquire);
        // This is to speed up cases where this thread just updated the Maps
        currMaps = maps.load(std::memory_order_relaxed);
        auto& currArr = currMaps->curr;
        auto initial = hash & (currArr.size - 1);
        for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
            auto ptr = currArr[i].load(std::memory_order_acquire);
            if (ptr.empty()) {
                if (currArr[i].compare_exchange_strong(ptr, ptrToInsert)) {
                    currMaps->curr.population.increment();
                    moveItems();
                    if (!checkCorrectMap(currMaps)) {
                        insert(key, value, hash, ptrToInsert);
                    }
                    return;
                }
            }
            if (ptr.alive()) {
                if (ptr->hash == hash && ptr->key == key) {
                    if (currArr[i].compare_exchange_strong(ptr, ptrToInsert)) {
                        if (!checkCorrectMap(currMaps)) {
                            insert(key, value, hash, ptrToInsert);
                        }
                        return;
                    }
                }
            }
            if (((i + 1) & (currArr.size - 1)) == initial) {
                throw std::runtime_error("Out of space in HashMap");
            }
        }
    }
    void resize(size_t oldCount) {
        auto currMaps = maps.load(std::memory_order_acquire);
        currMaps->ensureAllMoved();
        auto deleted = currMaps->curr.population.calculateDeletionCount();
        size_t newSize = 0;
        if (deleted <= oldCount / 2) {
            newSize = currMaps->curr.size * 2;
        } else if (deleted > oldCount * 3 / 4) {
            newSize = currMaps->curr.size / 2;
        } else {
            newSize = currMaps->curr.size;
        }
        newSize = std::max(newSize, 16uz);
        auto* newMap = new Maps::Map(newSize, *this);
        auto* newMaps = new Maps(newMap, &currMaps->curr);
        maps.compare_exchange_strong(currMaps, newMaps, std::memory_order_release, std::memory_order_relaxed);
    }
private:
    bool checkCorrectMap(Maps* assumedMaps) {
        return assumedMaps == this->maps.load(std::memory_order_acquire);
    }

    void moveItems() {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto index = currMaps->movingIndex.fetch_add(16);
        currMaps->moveItems(index);
    }
};
