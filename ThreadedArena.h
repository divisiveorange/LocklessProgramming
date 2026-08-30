#pragma once
#include <algorithm>
#include <atomic>
#include <cstdalign>
#include <optional>
#include <thread>
#include <type_traits>
#include <__new/interference_size.h>
#include <__thread/this_thread.h>
template <class... Ts>
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
    void* alloc(size_t n = 1) {
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
        ~LinkedMemory();
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
    ~LinkedMemory() {
        for (auto it = begin(); it < curr; ++it) {
            it->~T();
        }
    }
};

template <class... Ts>
class ThreadedArena {
    class PerHash;
    static constexpr size_t hashCapacity = 32;
    std::array<PerHash, hashCapacity> perHashes;
    inline static thread_local auto threadId = std::this_thread::get_id();
    inline static thread_local auto hash = std::hash<std::thread::id>{}(threadId) & (hashCapacity - 1);
    class alignas(std::hardware_destructive_interference_size) PerHash {
        class PerThread {
            struct alignas(std::hardware_destructive_interference_size) FirstCacheLineData {
                PerThread* _next{};
                std::thread::id _owningThread;
                FirstCacheLineData() : _next(nullptr), _owningThread(threadId) {}
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
            PerThread() = default;
            template<class T>
            void* alloc(size_t n = 1) {
                auto& head = std::get<T>(memoryHeads);
                if (!head) {
                    head = LinkedMemory<T>::construct(n);
                }
                if (auto memory = head->alloc(n)) {
                    return memory;
                }
                head = LinkedMemory<T>::construct(n, head);
                return alloc(n);
            }
            ~PerThread() {
                std::apply([](auto&... types) {
                    auto deletePerType = [](auto ptr) {
                        while (!ptr) {
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
    public:
        std::atomic<size_t> users{};
    private:
        PerThread* getThreadData() {
            for (auto it = head.load(std::memory_order_relaxed);
                it != nullptr;
                it = it.next) {
                if (it->owningThread == threadId) {
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
        void* alloc(size_t n = 1) {
            if (auto threadData = getThreadData()) {
                return threadData->template alloc<T>(n);
            }
            auto currHead = head.load(std::memory_order_relaxed);
            auto* next = new PerThread(currHead);
            while (!head.compare_exchange_weak(next->next(), next)) {
            }
            return next->alloc(n);
        }
        ~PerHash() {
            auto listPtr = head.load(std::memory_order_acquire);
            while (listPtr) {
                auto toDelete   = listPtr;
                listPtr = listPtr->next();
                delete toDelete;
            }
        }
    };
public:
    void markUsing() {
        perHashes[hash].markUsing();
    }
    void markNotUsing() {
        perHashes[hash].markNotUsing();
    }
    UsingToken<Ts...> getRAIIToken() {
        return {*this};
    }
    void* alloc() {
        return perHashes[hash].alloc();
    }
    bool safeToDelete() {
        for (auto& perHash : perHashes) {
            if (perHash.users.load(std::memory_order_acquire) > 0) {
                return false;
            }
        }
        return true;
    }
    ~ThreadedArena() {
        for (auto& perHash : perHashes) {
            perHash.~PerHash();
        }
    }

};

template<class... Ts>
class UsingToken {
    ThreadedArena<Ts...>* threadedArena;
    UsingToken(ThreadedArena<Ts...>* threadedArena) : threadedArena(threadedArena) {
        this->threadedArena->markUsing();
    }
    ~UsingToken() {
        if (threadedArena) {
            threadedArena->markNotUsing();
        }
    }
public:
    UsingToken(UsingToken&& other)  noexcept : threadedArena(other.threadedArena) {
        other.threadedArena == nullptr;
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