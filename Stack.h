#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#include <cassert>

template <typename T>
class Node;
template <typename T>
class RawStack;
template <typename T>
class CountedPtr {
public:
    uintptr_t iteration{};
    T* ptr{};
    bool operator==(const CountedPtr&) const = default;
    bool operator==(std::nullptr_t) const {
        return ptr == nullptr;
    }
    explicit operator bool() const {
        return not operator==(nullptr);
    }
    static CountedPtr null() {
        return {};
    }
    auto operator*() {
        return ptr;
    }
    auto operator->() {
        return ptr;
    }
};
template <typename T>
using NodePtr = CountedPtr<Node<T>>;
template <typename T>
class NodeHandle {
    NodePtr<T> node;
    RawStack<T>* nodeHome;
public:
    NodeHandle(const NodeHandle& other) = delete;
    NodeHandle(NodeHandle&& other) : node(other.node), nodeHome(other.nodeHome) {
        other.nodeHome = nullptr;
        other.node = NodePtr<T>::null();
    }
    NodeHandle(NodePtr<T> node, RawStack<T>& nodeHome) : node(node), nodeHome(&nodeHome) {
    }
    T& get() {
        return node->get();
    }
    ~NodeHandle() {
        if (nodeHome) {
            node->unSet();
            nodeHome->push(node);
        }
    }
};
template <typename T>
class alignas(64) Node {
public:
    void unSet() {
        if (isSet) {
            get().~T();
            isSet = false;
        }
    }
    template <typename Arg>
    void set(Arg&& value) {
        unSet();
        isSet = true;
        new(&rawData)T(std::forward<Arg>(value));
    }
    T& get() {
        return *(reinterpret_cast<T*>(&rawData));
    }
    Node() {}
    template <typename Arg>
    Node(Arg&& value) {
        set(std::forward<Arg>(value));
    }
    bool isSet{};
    alignas(T) char rawData[sizeof(T)];
    NodePtr<T> next{};
    ~Node() {
        unSet();
    }
};
template <typename T>
class RawStack {
    std::atomic<NodePtr<T>> head;
public:
    void push(NodePtr<T> newNode) {
        newNode->next = head.load(std::memory_order_relaxed);
        assert(newNode.ptr != newNode->next.ptr);
        while (!head.compare_exchange_weak(newNode->next, newNode)) {
        }
    }
    NodePtr<T> pop() {
        auto result = head.load(std::memory_order_relaxed);
        if (!result) {
            return result;
        }
        assert(result.ptr != result->next.ptr);
        while (result && !head.compare_exchange_weak(result, result->next)) {
        }
        ++result.iteration;
        return result;
    }
    ~RawStack() {
        while (pop() != NodePtr<T>::null()) {
            delete pop().ptr;
        }
    }
};
template <typename T>
class Stack {
private:
    RawStack<T> dead;
    RawStack<T> alive;
public:
    std::optional<NodeHandle<T>> pop() {
        if (NodePtr<T> nodePtr = alive.pop()) {
            return NodeHandle<T>(nodePtr, dead);
        }
        return {};
    }
    template <typename Arg>
    void push(Arg&& value) {
        NodePtr<T> nodePtr = dead.pop();
        if (!nodePtr) {
            Node<T>* newNodeRawPtr = new Node<T>(std::forward<Arg>(value));
            nodePtr = NodePtr<T>{0, newNodeRawPtr};
        } else {
            nodePtr->set(std::forward<Arg>(value));
        }
        alive.push(nodePtr);
    }
};


