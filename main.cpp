#include <iostream>
#include <vector>
#include <__thread/jthread.h>

#include "Stack.h"

void doStuff(Stack<int>& stack) {
    for (auto i = 0ll; i < 100000; i++) {
        if (i % 2 == 0) {
            auto handle = stack.pop();
        }
        stack.push(i);
    }
}


int main() {
    Stack<int> stack;
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 15; i++) {
            threads.emplace_back([&stack]() {
                doStuff(stack);
            });
        }
    }
    while (auto node = stack.pop()) {
        std::cout << node->get() << std::endl;
    }
    return 0;
}
