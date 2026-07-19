#pragma once
#include <atomic>
#include <stdexcept>
#include <algorithm>
template<typename K, typename V>
class ResizeableHashMap;

template<class K, class V>
class alignas(64) Payload {
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
class alignas(64) PackedPointer {
    // The pointer part of this should be treated as const.
    std::uintptr_t value;

public:
    PackedPointer(Payload *value = nullptr) : value(reinterpret_cast<std::uintptr_t>(value)) {
    }

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
            std::atomic<Ptr> *data;
            const size_t size;
            std::atomic<size_t> moving_index{};
        };

        Map curr, old;
    };

private:
    std::atomic<Maps *> maps;

public:
    Payload<K, V>* get(const K &key) {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto hash = std::hash<K>{}(key);
        auto currArr = currMaps->curr.data;
        {
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
        currArr = currMaps->old.data;
        PackedPointer<Payload<K,V>> targetPtr{};
        {
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
                        targetPtr == ptr.ptr();
                        break;
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return nullptr;
                }
            }
        }
        currArr = currMaps->curr.data;
        {
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
        auto currMaps = maps.load(std::memory_order_acquire);
        auto hash = std::hash<K>{}(key);
        auto currArr = currMaps->curr.data;
        {
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
                            return true;
                        }
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    break;
                }
            }
        }
        currArr = currMaps->old.data;
        PackedPointer<Payload<K,V>> targetPtr{};
        {
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
                            return true;
                        }
                    }
                }
                if (ptr.beingMoved()) {
                    if (ptr->hash == hash && ptr->key == key) {
                        targetPtr == ptr.ptr();
                        break;
                    }
                }
                if (((i + 1) & (currArr.size - 1)) == initial) {
                    return false;
                }
            }
        }
        currArr = currMaps->curr.data;
        {
            auto initial = hash & (currArr.size - 1);
            for (auto i = initial; ; i = (i + 1) & (currArr.size - 1)) {
                auto ptr = currArr[i].load(std::memory_order_acquire);
                if (ptr.empty()) {
                    auto deadPtr = targetPtr.makeDead();
                    if (currArr[i].compare_exchange_strong(ptr, deadPtr)) {
                        return true;
                    }
                }
                if (ptr.ptr() == targetPtr.ptr()) {
                    if (ptr.alive()) {
                        auto deadPtr = targetPtr.makeDead();
                        return currArr[i].compare_exchange_strong(ptr, deadPtr);
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
    }
private:
    void moveItems() {
        auto currMaps = maps.load(std::memory_order_acquire);
        auto index = currMaps->old.moving_index.fetch_add(16);
        if (index < currMaps->old.size) {
            auto begin = &currMaps->old.data[index];
            auto end = &currMaps->old.data[std::max(index + 16, currMaps->old.size)];
            for (auto it = begin; begin != end; ++begin) {
                auto ptr = it->load(std::memory_order_acquire);
                if (ptr.alive()) {
                    auto movedPtr = ptr; movedPtr.markMoved();
                    if (it->compare_exchange_strong(ptr, movedPtr)) {
                        moveInsert(ptr, currMaps->curr);
                    }
                }
            }
        }
    }
    void moveInsert(PackedPointer<Payload<K,V>> ptr, Maps::Map& map) {
        auto initial = ptr->hash & (map.size - 1);
        for (auto i = initial; ; i = (i+1) & (map.size - 1)) {
            auto slot = map.data[i].load(std::memory_order_acquire);
            if (slot.empty()) {
                if (map.data[i].compare_exchange_strong(slot, {ptr.ptr()})) {
                    return;
                }
            }
            if (slot->hash == ptr->hash && slot->key == ptr->hash) {
                return;
            }
            if (initial == ((i+1) & (map.size -1 ))) {
                throw std::runtime_error("Out of space in HashMap");
            }
        }
    }
};
