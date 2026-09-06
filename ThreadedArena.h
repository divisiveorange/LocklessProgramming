#pragma once
#include <algorithm>
#include <atomic>
#include <cstdalign>
#include <list>
#include <optional>
#include <thread>
#include <type_traits>
#include <__new/interference_size.h>
#include <__thread/this_thread.h>
class UsingToken;
template <class T>
class LinkedMemory {
    static constexpr size_t defaultSize = 4096;
public:
    const size_t size;
    LinkedMemory* next{};
    T* curr{};
    // Raw pointer because deallocation shouldn't be deeply recursive due to stack overflows.
    T* begin() {
        return reinterpret_cast<T*>(&_data[0]);
    }
    T* end() {
        return reinterpret_cast<T*>(begin() + size);
    }
    void* alloc(size_t n) {
        auto newCurr = curr + n;
        if (newCurr <= end()) {
            auto start = curr;
            curr = newCurr;
            return start;
        }
        return nullptr;
    }
    static LinkedMemory* construct(size_t size = defaultSize, LinkedMemory* next = nullptr) {
        auto roundedSize = ((size - 1)/defaultSize+1)*(defaultSize);
        void* memory = ::operator new(sizeof(LinkedMemory) + roundedSize * sizeof(T), static_cast<std::align_val_t>(alignof(T)));
        return new(memory) LinkedMemory(roundedSize, next);
    }
    static LinkedMemory* construct(LinkedMemory* next) {
        return construct(defaultSize, next);
    }
    void destroy() {
        this->~LinkedMemory();
        ::operator delete(this);
    }
private:
    class RawObject {
    public:
        alignas(alignof(T)) std::byte raw[sizeof(T)];
    };
    // I'm aware trailing struct arrays aren't part of standard C++, but it's the cleanest way to do this in my opinion.
    RawObject _data[];
    LinkedMemory(size_t size, LinkedMemory* next) : size(size), next(next) {
        curr = begin();
    }
    ~LinkedMemory() requires std::is_trivially_destructible_v<T> = default;
    ~LinkedMemory() {
        for (auto it = begin(); it < curr; ++it) {
            it->~T();
        }
    }
};
class UsingTracker {
// The purpose of this class is to allow UsingToken to not be templated while ThreadedArena needs to be
public:
    virtual void markUsing() = 0;
    virtual void markNotUsing() = 0;
    virtual ~UsingTracker() = default;
};


class UsingToken {
    UsingTracker* threadedArena;
public:
    UsingToken(UsingTracker* threadedArena) : threadedArena(threadedArena) {
        this->threadedArena->markUsing();
    }
    ~UsingToken() {
        if (threadedArena) {
            threadedArena->markNotUsing();
        }
    }
    UsingToken(UsingToken&& other)  noexcept : threadedArena(other.threadedArena) {
        other.threadedArena = nullptr;
    }
    auto& operator=(UsingToken&& other)  noexcept {
        if (threadedArena != other.threadedArena) {
            this->~UsingToken();
            threadedArena = other.threadedArena;
            other.threadedArena = nullptr;
        }
        return *this;
    }
};
template <class... Ts>
class ThreadedArena : public UsingTracker {
public:
    static constexpr size_t arenaCount = 3;
    class PerHash;
    static constexpr size_t hashCapacity = 32;
    std::array<PerHash, hashCapacity> perHashes;
    inline static thread_local auto threadId = std::this_thread::get_id();
    inline static thread_local auto hash = std::hash<std::thread::id>{}(threadId) & (hashCapacity - 1);
    class alignas(std::hardware_destructive_interference_size) PerHash {
    public:
        class PerThread {
            struct alignas(std::hardware_destructive_interference_size) FirstCacheLineData {
                PerThread* _next{};
                std::thread::id _owningThread;
                FirstCacheLineData(PerThread* next = nullptr) : _next(next), _owningThread(threadId) {}
            } firstCacheLineData;
            // So this is the data that is accessed by other threads, the data modified and accessed by only 1 thread
            // can go on another cacheline to prevent contention because it is edited frequently.
            std::tuple<LinkedMemory<Ts>*...> memoryHeads{};
        public:
            PerThread*& next() {
                return firstCacheLineData._next;
            }
            std::thread::id& owningThread() {
                return firstCacheLineData._owningThread;
            }
            PerThread(PerThread* next = nullptr) : firstCacheLineData(next) {
            }
            template<class T>
            void* alloc(size_t n) {
                auto& head = std::get<LinkedMemory<T>*>(memoryHeads);
                if (!head) {
                    head = LinkedMemory<T>::construct(n);
                }
                if (auto memory = head->alloc(n)) {
                    return memory;
                }
                head = LinkedMemory<T>::construct(n, head);
                return alloc<T>(n);
            }
            ~PerThread() {
                std::apply([](auto&... types) {
                    auto deletePerType = [](auto ptr) {
                        while (ptr) {
                            auto toDelete = ptr;
                            ptr = ptr->next;
                            toDelete->destroy();
                        }
                    };
                    (deletePerType(types), ...);
                }, memoryHeads);
            }
        };
        std::atomic<PerThread*> head;
        std::atomic<size_t> users{};
    private:
        PerThread* getThreadData() {
            for (auto it = head.load(std::memory_order_relaxed);
                it != nullptr;
                it = it->next()) {
                if (it->owningThread() == threadId) {
                    return it;
                }
            }
            return nullptr;
        }
    public:
        void markUsing() {
            users.fetch_add(1, std::memory_order_release);
        }
        void markNotUsing() {
            users.fetch_sub(1, std::memory_order_release);
        }
        template <class T>
        void* alloc(size_t n) {
            if (auto threadData = getThreadData()) {
                return threadData->template alloc<T>(n);
            }
            auto currHead = head.load(std::memory_order_relaxed);
            auto* next = new PerThread(currHead);
            while (!head.compare_exchange_weak(next->next(), next)) {
            }
            return next->template alloc<T>(n);
        }
    };
    void markUsing() override {
        perHashes[hash].markUsing();
    }
    void markNotUsing() override {
        perHashes[hash].markNotUsing();
    }
public:
    UsingToken getRAIIToken() {
        return {this};
    }
    std::atomic<int> perpetualUsers{};
    template <class T>
    void* alloc(size_t n = 1) {
        return perHashes[hash].template alloc<T>(n);
    }
    bool safeToDelete() {
        for (auto& perHash : perHashes) {
            if (perHash.users.load(std::memory_order_acquire) > 0) {
                return false;
            }
        }
        return true;
    }
    ThreadedArena() = default;
    ~ThreadedArena() override {
        for (auto& perHash : perHashes) {
            auto listPtr = perHash.head.load(std::memory_order_acquire);
            while (listPtr) {
                auto toDelete = listPtr;
                listPtr = listPtr->next();
                delete toDelete;
            }
        }
    }
    std::atomic<size_t> iteration{};
    void setForReuse(size_t expectedIteration) {
        std::vector<typename ThreadedArena::PerHash::PerThread*> vec;
        // size_t expectedIteration = iteration.load(std::memory_order_acquire);
        for (auto& perHash : perHashes) {
            auto listPtr = perHash.head.load(std::memory_order_acquire);
            if (!listPtr) continue;
            if (iteration.load(std::memory_order_acquire) != expectedIteration) return;
            if (perHash.head.compare_exchange_strong(listPtr, nullptr)) {
                vec.push_back(listPtr);
            }
        }
        iteration.compare_exchange_strong(expectedIteration, expectedIteration + arenaCount);
        for (auto listPtr : vec) {
            while (listPtr) {
                auto toDelete   = listPtr;
                listPtr = listPtr->next();
                delete toDelete;
            }
        }
    }
    // To implement later: partial delete so one can spread out the deallocation.

};