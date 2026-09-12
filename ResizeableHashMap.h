#pragma once
#include <atomic>
#include <stdexcept>
#include <algorithm>
#include <thread>
#include <__new/interference_size.h>
#include <__thread/this_thread.h>
#include "ThreadedArena.h"
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
        return !(value & 1) and !empty();
    }
    bool dead() {
        return value & 1;
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
    template <class T>
    class LazyHeapCopy {
        std::optional<T*> lazyCopy;
        T* ptr;
        ResizeableHashMap& map;
    public:
        LazyHeapCopy(T* ptr, ResizeableHashMap& map) : ptr(ptr), map(map) {}
        T* getCopy() {
            if (lazyCopy) return *lazyCopy;
            lazyCopy = new(map.alloc<T>()) T(*ptr);
            return *lazyCopy;
        }
    };
    class Maps;
    using Ptr = PackedPointer<Payload<K, V> >;
    using Arena = ThreadedArena<Payload<K,V>, std::atomic<Ptr>, std::atomic<bool>, Maps, typename Maps::Map>;
    class Maps {
    public:
        class Map {
            ResizeableHashMap& fullMap;
        public:
            std::atomic<Ptr>* data;
            const size_t size;
            size_t createdEpoch{};
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
                        counters[index].fetch_add(incrementSize, std::memory_order_acq_rel);
                        if (totalCount + incrementSize >= fullMap.maps.load(std::memory_order_relaxed)->curr->size / 2) {
                            fullMap.resize(totalCount + incrementSize);
                        }
                    }
                }
                void decrement() {
                    auto index = std::hash<std::thread::id>{}(std::this_thread::get_id()) & (threadHashSize-1);
                    deletions[index].fetch_add(1, std::memory_order_release);
                }
            } population{};
            bool moveInsert(Payload<K,V>* ptr) {
                LazyHeapCopy<Payload<K,V>> lazyCopy(ptr, fullMap);
                auto initial = ptr->hash & (size - 1);
                for (auto i = initial; ; i = (i+1) & (size - 1)) {
                    auto slot = data[i].load(std::memory_order_acquire);
                    if (slot.empty()) {
                        population.increment();
                        if (data[i].compare_exchange_strong(slot, {lazyCopy.getCopy()})) {
                            return true;
                        }
                    }
                    if (slot->hash == ptr->hash && slot->key == ptr->key) {
                        return false;
                    }
                    if (initial == ((i+1) & (size -1 ))) {
                        throw std::runtime_error("Out of space in HashMap");
                    }
                }
            }
            bool checkAllMoved() {
                for (size_t i = 0; i < size; i++) {
                    if (data[i].load(std::memory_order_acquire).alive()) {
                        return false;
                    }
                }
                return true;
            }
            Map(size_t size, ResizeableHashMap& fullMap, int createdEpoch) : fullMap(fullMap), size(size), population(fullMap), createdEpoch(createdEpoch) {
                data = new(fullMap.alloc<std::atomic<Ptr>>(size)) std::atomic<Ptr>[size];
            }
            class iterator {
                std::atomic<Ptr> *curr, *end;
            public:
                iterator(std::atomic<Ptr>* curr, std::atomic<Ptr>* end) : curr(curr), end(end) {
                    if (!curr->load(std::memory_order_acquire).alive()) {
                        operator++();
                    }
                }
                auto operator++() {
                    while (curr != end) {
                        ++curr;
                        if (curr->load(std::memory_order_acquire).alive()) {
                            return;
                        }
                    }
                    if (curr > end) {
                        curr = end;
                    }
                }
                auto operator*() {
                    return curr->load(std::memory_order_acquire).ptr();
                }
                auto operator<=>(const iterator& other) {
                    return curr <=> other.curr;
                }
                auto operator!=(const iterator& other) {
                    return curr != other.curr;
                }
            };
            iterator begin() {
                return iterator(data, data + size);
            }
            iterator end() {
                return iterator(data + size, data + size);
            }
            bool contains(const K& key) {
                for (auto it = begin(); it != end(); ++it) {
                    auto item = *it;
                    if (item->key == key) {
                        return true;
                    }
                }
                return false;
            }
        };
        Map* curr,* old;
        ResizeableHashMap& fullMap;
        std::atomic<size_t> movingIndex{};
        std::atomic<bool>* movedChunks;
        Maps(Map* curr, Map* old, ResizeableHashMap& fullMap) : curr(curr), old(old), fullMap(fullMap) {
            movedChunks = new(fullMap.alloc<std::atomic<bool>>(this->old->size / 16)) std::atomic<bool>[this->old->size / 16]{};
        }
        Maps(size_t oldSize, size_t newSize, ResizeableHashMap& fullMap) : curr(new(fullMap.alloc<Map>()) Map(newSize, fullMap, 0)), old(new(fullMap.alloc<Map>()) Map(oldSize, fullMap, 0)) , fullMap(fullMap) {
            movedChunks = new(fullMap.alloc<std::atomic<bool>>(this->old->size / 16)) std::atomic<bool>[this->old->size / 16]{};
        }
        void moveItems(size_t index) {
            if (index < old->size) {
                auto begin = &old->data[index];
                auto end = &old->data[std::min(index + 16, old->size)];
                for (auto it = begin; it < end; ++it) {
                    auto ptr = it->load(std::memory_order_acquire);
                    if (not ptr.dead() and not ptr.empty()) {
                        auto movedPtr = ptr; movedPtr.markMoved();
                        if (it->compare_exchange_strong(ptr, movedPtr)) {
                            curr->moveInsert(ptr.ptr());
                            if (!fullMap.checkCorrectMap(this)) {
                                fullMap.maps.load(std::memory_order_acquire)->moveItems(index);
                                return;
                            }
                            auto killedPtr = movedPtr; killedPtr.kill();
                            it->compare_exchange_strong(movedPtr, killedPtr);
                        }
                    }
                }
                movedChunks[index / 16].store(true);
            }
        }
        void ensureAllMoved() {
            for (size_t i = 0; i < old->size; i += 16) {
                if (not movedChunks[i/16].load(std::memory_order_acquire)) {
                    moveItems(i);
                }
            }
        }
    };
    std::atomic<Maps *> maps;

public:
    ResizeableHashMap() {
        maps = new(alloc<Maps>()) Maps(16, 128 , *this);
    }
    Payload<K, V>* get(const K &key, UsingToken& token) const {
        return get(key, std::hash<K>{}(key));
    }
    Payload<K, V>* get(const K &key, size_t hash) const {
        auto currMaps = maps.load(std::memory_order_acquire);
        {
            auto& currArr = *currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    break;
                }
                if (ptr.beingMoved()) {
                    return get(key, hash);
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        if (!checkCorrectMap(currMaps)) {
                            return get(key, hash);
                        }
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
            auto& currArr = *currMaps->old;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    if (!checkCorrectMap(currMaps)) {
                        return get(key, hash);
                    }
                    return nullptr;
                }
                if (ptr.beingMoved()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        targetPtr = ptr.ptr();
                        break;
                    }
                }
                if (ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        if (!checkCorrectMap(currMaps)) {
                            return get(key, hash);
                        }
                        return ptr.ptr();
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return nullptr;
                }
            }
        }
        {
            auto& currArr = *currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    return targetPtr.ptr();
                }
                if (ptr->hash == targetPtr->hash and ptr->key == targetPtr->key) {
                    if (ptr.alive()) {
                        if (!checkCorrectMap(currMaps)) {
                            return get(key, hash);
                        }
                        return ptr.ptr();
                    }
                    if (!checkCorrectMap(currMaps)) {
                        return get(key, hash);
                    }
                    return nullptr;
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    if (!checkCorrectMap(currMaps)) {
                        return get(key, hash);
                    }
                    return targetPtr.ptr();
                }
            }
        }
    }
    void remove(const K &key, UsingToken& token) {
       remove(key, std::hash<K>{}(key));
    }
private:
    void remove(const K &key, const size_t hash) {
        auto currMaps = maps.load(std::memory_order_acquire);
        bool removed = false;
        {
            auto& currArr = *currMaps->curr;
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
                                remove(key, hash);
                                return;
                            }
                            currMaps->curr->population.decrement();
                            removed = true;
                            break;
                        }
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    throw std::runtime_error("Out of space in HashMap");
                }
            }
        }
        Payload<K,V>* target{};
        {
            auto& currArr = *currMaps->old;
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    if (!checkCorrectMap(currMaps)) {
                        remove(key, hash);
                        return;
                    }
                    return;
                }
                if (not ptr.beingMoved() and ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        auto deadPtr = ptr.makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                remove(key, hash);
                                return;
                            }
                            return;
                        }
                    }
                }
                if (ptr.beingMoved() and ptr.alive()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        target = ptr.ptr();
                        auto deadPtr = ptr.makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                remove(key, hash);
                                return;
                            }
                        }
                        break;
                    }
                }
                if (ptr.beingMoved() and ptr.dead()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        target = ptr.ptr();
                        break;
                    }
                }

                if (((i + 1) & (currArr.size - 1)) == initial) {
                    throw std::runtime_error("Out of space in HashMap");
                }
            }
        }
        {
            auto& currArr = *currMaps->curr;
            auto initial = hash & (currArr.size - 1);
            LazyHeapCopy<Payload<K,V>> lazyCopy(target, *this);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    auto deadPtr = PackedPointer(lazyCopy.getCopy()).makeDead();
                    currMaps->curr->population.increment();
                    moveItems();
                    if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                        if (!checkCorrectMap(currMaps)) {
                            remove(key, hash);
                            return;
                        }
                        currMaps->curr->population.decrement();
                        return;
                    }
                }
                if (ptr->hash == target->hash and ptr->key == target->key) {
                    if (ptr.alive()) {
                        auto deadPtr = PackedPointer(lazyCopy.getCopy()).makeDead();
                        if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                            if (!checkCorrectMap(currMaps)) {
                                remove(key, hash);
                                return;
                            }
                            currMaps->curr->population.decrement();
                            return;
                        }
                        if (!checkCorrectMap(currMaps)) {
                            remove(key, hash);
                        }
                        return;

                    }
                    if (!checkCorrectMap(currMaps)) {
                        remove(key, hash);
                    }
                    return;
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    throw std::runtime_error("Out of space in HashMap");
                }
            }
        }
    }
public:
    void insert(const K &key, const V &value, UsingToken& token) {
        auto hash = std::hash<K>{}(key);
        insert(key, value, hash);
    }
private:
    void insert(const K &key, const V &value, size_t hash) {
        auto currMaps = maps.load(std::memory_order_acquire);
        Ptr ptrToInsert(new(alloc<Payload<K,V>>()) Payload<K, V>(key, value, hash));
        auto& currArr = *currMaps->curr;
        auto initial = hash & (currArr.size - 1);
        for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
            auto ptr = currArr[i].load(std::memory_order_acquire);
            if (ptr.empty()) {
                currMaps->curr->population.increment();
                moveItems();
                if (currArr[i].compare_exchange_strong(ptr, ptrToInsert)) {
                    if (!checkCorrectMap(currMaps)) {
                        insert(key, value, hash);
                    }
                    return;
                }
            }
            if (ptr.alive()) {
                if (ptr->hash == hash && ptr->key == key) {
                    if (currArr[i].compare_exchange_strong(ptr, ptrToInsert)) {
                        if (!checkCorrectMap(currMaps)) {
                            insert(key, value, hash);
                        }
                        return;
                    }
                }
            }
            if (((i + 1) & (currArr.size - 1)) == initial) {
                resize(currMaps->curr->size);
                insert(key, value, hash);
            }
        }
    }
    void resize(size_t oldCount) {
        tryAdvanceEpoch();
        // A relaxed load because it did an acquire load in the previous function
        auto epoch = currentEpoch.load(std::memory_order_relaxed);
        auto currMaps = maps.load(std::memory_order_acquire);
        currMaps->ensureAllMoved();
        auto* oldMap = currMaps->old;
        auto deleted = currMaps->curr->population.calculateDeletionCount();
        size_t newSize = 0;
        if (deleted <= oldCount / 2) {
            newSize = currMaps->curr->size * 2;
        } else if (deleted > oldCount * 3 / 4) {
            newSize = currMaps->curr->size / 2;
        } else {
            newSize = currMaps->curr->size;
        }
        newSize = std::max(newSize, 16uz);
        auto* newMap = new(arenas[epoch % arenas.size()].template alloc<typename Maps::Map>()) Maps::Map(newSize, *this, epoch);
        auto* newMaps = new(arenas[epoch % arenas.size()].template alloc<Maps>()) Maps(newMap, currMaps->curr, *this);
        if (!newMaps->old) {

        }
        auto* prevCurr = currMaps->curr;
        if (maps.compare_exchange_strong(currMaps, newMaps)) {
            arenas[epoch % arenas.size()].perpetualUsers.fetch_add(1, std::memory_order_release);
            arenas[oldMap->createdEpoch % arenas.size()].perpetualUsers.fetch_sub(1, std::memory_order_release);
        }
    }
    bool tryAdvanceEpoch() {
        auto curr= currentEpoch.load(std::memory_order_acquire);
        auto nextIndex = (curr + 1) % arenas.size();
        if (arenas[nextIndex].safeToDelete()) {
            arenas[nextIndex].setForReuse(curr+1-arenas.size());
            return currentEpoch.compare_exchange_strong(curr, curr + 1, std::memory_order_acq_rel);
        }
        return false;
    }
    auto getCurrMap() {
        return &(maps.load()->curr);
    }
public:
    auto createUsingToken() {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto token = arenas[currMaps->old->createdEpoch % arenas.size()].getRAIIToken();
        if (!checkCorrectMap(currMaps)) {
            return createUsingToken();
        }
        return token;
    }
private:
    bool checkCorrectMap(Maps* assumedMaps) const {
        return assumedMaps == this->maps.load(std::memory_order_acquire);
    }
    void moveItems() {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto index = currMaps->movingIndex.fetch_add(16);
        currMaps->moveItems(index);
    }
    std::atomic<size_t> currentEpoch{};
    size_t loadCurrEpochIndex() {
        return currentEpoch.load(std::memory_order_acquire) % arenas.size();
    }
    template <typename T>
    void* alloc(size_t n = 1) {
        auto ptr = arenas[loadCurrEpochIndex()].template alloc<T>(n);
        if (!ptr) {

        }
        return ptr;
    }
    std::array<Arena, Arena::arenaCount> arenas;

};
