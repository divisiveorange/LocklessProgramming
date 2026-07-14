#pragma once

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
        return reinterpret_cast<Payload *>(value & (~3));
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
    void beingMoved() {
        return value & 2;
    }
    void markMoved() {
        value |= 2;
    }
    operator bool() const {
        return value;
    }
};
