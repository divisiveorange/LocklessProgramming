#pragma once
#include "Stack.h"
/*
template <typename Node>
class BasicStack {
    std::atomic<CountedPtr<Node>> head;
public:
    CountedPtr<Node> pop() {
        auto oldHead = head.load(std::memory_order_relaxed);
        while (oldHead && !head.compare_exchange_weak(oldHead, oldHead->next, std::memory_order_acquire, std::memory_order_relaxed)) {
            head.
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
template <typename Node>
class FreeList {
    BasicStack<Node> stack;
public:
    auto retire(CountedPtr<Node> value) {
        stack.push(value);
    }
    auto get() {
        auto result = stack.pop();
        if (!result) {
            result.iteration = 0;
            result.ptr = new Node();
        }
        return result;
    }
};

template <typename T>
class DequeNode;
template <typename T>
using DequeNodePtr = CountedPtr<DequeNode<T>>;
template <typename T>
class DequeNode {
    alignas(T) char data [sizeof(T)];
public:
    DequeNodePtr<T> prev;
    DequeNodePtr<T> next;
    std::atomic<bool> removed{};

    T& get() {
        return *reinterpret_cast<T*>(&data);
    }
    template <typename Args>
    void set(Args&& arg) {
        if (isSet) {
            get().~T();
            isSet = true;
        }
        new(&data) T(std::forward<Args>(arg));
    }
    ~DequeNode() {
        if (isSet) {
            get().~T();
        }
    }
private:
    bool isSet{};
};
template <typename T>
class Deque {
    /*
     Notes:
     for pushing:
        make node,
        get head,
        if the node is to be removed, go backwards
        Compare head->next == null and swap with new node,
        else get the next item and update the head pointer
        when successful, try update the head pointer (if it's not already where it should be).

     for popping:

     *//*
    std::atomic<DequeNodePtr<T>> front;
    std::atomic<DequeNodePtr<T>> back;
    FreeList<DequeNode<T>> freeList;
public:
    void pushFront(const T& value) {
        auto oldFront = front.load(std::memory_order_relaxed);
    }
    T popFront() {

    }
    void pushBack(const T& value) {

    }
    T popBack() {

    }

};
*/