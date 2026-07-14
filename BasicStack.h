#pragma once
#include "Stack.h"

template <typename Node>
class BasicStack {
    std::atomic<CountedPtr<Node>> head;
public:
    CountedPtr<Node> pop() {
        auto oldHead = head.load(std::memory_order_relaxed);
        while (oldHead && !head.compare_exchange_weak(oldHead, oldHead->next, std::memory_order_acquire, std::memory_order_relaxed)) {
        }
        ++oldHead.iteration;
        return oldHead;
    }
    void push(CountedPtr<Node> value) {
        auto oldHead = head.load(std::memory_order_relaxed);
        value->next = oldHead;
        while (!head.compare_exchange_weak(value->next, value, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
    ~BasicStack() {
        while (auto node = pop()) {
            delete node.ptr;
        }
    }
};