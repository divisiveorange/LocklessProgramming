#pragma once
#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#include <type_traits>
#include <__new/interference_size.h>
#include <__thread/this_thread.h>
template <class T>
class UsingToken;
template <class T>
class Epoch {
    class PerHash;
    static constexpr size_t hashCapacity = 32;
    std::array<PerHash, hashCapacity> perHashes;
    inline static thread_local auto threadId = std::this_thread::get_id();
    inline static thread_local auto hash = std::hash<std::thread::id>{}(threadId) & (hashCapacity - 1);
    class alignas(std::hardware_destructive_interference_size) PerHash {
        class PerThread {
        public:
            PerThread* next{};
            std::thread::id owningThread;
        };
        std::atomic<PerThread*> head;
        std::atomic<size_t> users{};
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
        void retire(T* item) {
            auto threadData = getThreadData();
            if (!threadData) {

            }
        }
    };
public:
    void retire(T* item) {
        perHashes[hash].retire(item);
    }
    void markUsing() {
        perHashes[hash].markUsing();
    }
    void markNotUsing() {
        perHashes[hash].markNotUsing();
    }
    UsingToken<T> getRAIIToken() {
        return {*this};
    }
};

template<class T>
class UsingToken {
    Epoch<T>* epoch;
    UsingToken(Epoch<T>* epoch) : epoch(epoch) {
        this->epoch->markUsing();
    }
    ~UsingToken() {
        if (epoch) {
            epoch->markNotUsing();
        }
    }
public:
    UsingToken(UsingToken&& other)  noexcept : epoch(epoch) {
        other.epoch == nullptr;
    }
    auto& operator=(UsingToken&& other)  noexcept {
        if (epoch != other.epoch) {
            this->~UsingToken();
            epoch = other.epoch;
            other.epoch = nullptr;
        }
        return *this;
    }
};