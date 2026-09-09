#include <iostream>
#include <vector>
#include <__thread/jthread.h>
#include "ResizeableHashMap.h"

#include "Stack.h"
static constexpr int size = 1000;
static constexpr int threadCount = 16;
template <typename K, typename V>
void doStuff(ResizeableHashMap<K, V>& map, int threadIndex) {
    for (auto i = 0ll; i < size; i++) {
        auto token = map.createUsingToken();
        map.insert(i, threadIndex, token);
        std::this_thread::sleep_for(std::chrono::nanoseconds(10000));
        map.remove(size-i, token);
    }
}
bool testMemoryReclaimation() {
    ResizeableHashMap<int, bool> map;
    std::vector<std::jthread> threads;
    for (int i = 0; i < threadCount; i++) {
        threads.emplace_back([&map, i]() {
            for (int k = 0; k < size; k++) {
                for (int j = 0; j < size; j++) {
                    auto token = map.createUsingToken();
                    map.insert(size * i + j, true, token);
                }
                for (int j = 0; j < size; j++) {
                    auto token = map.createUsingToken();
                    if (!map.get(size * i + j, token)) {
                        std::cout << size * i + j << " wasn't inserted\n";
                    }
                }
                for (int j = 0; j < size; j++) {
                    auto token = map.createUsingToken();
                    map.remove(size * i + j, token);
                }
                for (int j = 0; j < size; j++) {
                    auto token = map.createUsingToken();
                    if (map.get(size * i + j, token)) {
                        std::cout << size * i + j << " wasn't removed\n";
                    }
                }
            }
        });
    }
    return true;
}
bool testDataRemains() {
    for (int k = 0; k < 1000; k++) {
        std::cout << "Iteration: " << k << "\n";
        ResizeableHashMap<int, bool> map;
        {
            std::vector<std::jthread> threads;
            threads.reserve(threadCount);
            for (int i = 0; i < threadCount; i++) {
                threads.emplace_back([&map, i]() {
                    for (int j = 0; j < size; j++) {
                        auto token = map.createUsingToken();
                        map.insert(size*i + j, true, token);
                        if (j >= 4) {
                            if (!map.get(size*i+j-4, token)) {
                                std::cout << size*i+j-4 << " is missing\n";
                                return false;
                            }
                        }
                    }
                return true;
                });
            }
        }
        {
            auto token = map.createUsingToken();
            for (int i = 0; i < threadCount; i++) {
                for (int j = size; j < size; j++) {
                    if (!map.get(size*i+j, token)) {
                        std::cout << size*i+j << " Was removed\n";
                        return false;
                    }
                }
            }
        }
        {
            std::vector<std::jthread> threads;
            threads.reserve(threadCount);
            for (int i = 0; i < threadCount; i++) {
                threads.emplace_back([&map, i]() {
                    for (int j = 0; j < size/2; j++) {
                        auto token = map.createUsingToken();
                        map.remove(size*i + j, token);
                        if (map.get(size*i+j, token)) {
                                std::cout << size*i+j << " (just removed) wasn't removed\n";
                                std::this_thread::sleep_for(std::chrono::nanoseconds(1000000));
                                if (map.get(size*i+j, token)) {
                                    std::cout << "Get is correct, remove is wrong" << std::endl;
                                } else {
                                    std::cout << "Get is wrong, remove is correct" << std::endl;
                                }
                                return false;
                        }
                        map.insert(threadCount*size + j, true, token);
                        if (j >= 4) {
                            if (map.get(size*i+j-4, token)) {
                                std::cout << size*i+j-4 << " (-4) wasn't removed\n";
                                std::this_thread::sleep_for(std::chrono::nanoseconds(1000000));
                                if (map.get(size*i+j - 4, token)) {
                                    std::cout << "Get is correct, remove is wrong" << std::endl;
                                } else {
                                    std::cout << "Get is wrong, remove is correct" << std::endl;
                                }
                                return false;
                            }
                        }
                    }
                    return true;
                });
            }
        }
        for (int i = 0; i < threadCount; i++) {
            for (int j = 0; j < size/2; j++) {
                auto token = map.createUsingToken();
                if (map.get(size*i+j, token)) {
                    std::cout << size*i+j << " Remains\n";
                    return false;
                }
            }
            for (int j = size/2; j < size; j++) {
                auto token = map.createUsingToken();
                if (!map.get(size*i+j, token)) {
                    std::cout << size*i+j << " Was removed\n";
                    return false;
                }
            }
        }
    }
    return true;
}
void smokeTest() {
    // My goal here is to write code, I can't really be bothered to write super robust testing.
    ResizeableHashMap<int, int> map;
    auto token = map.createUsingToken();
    map.insert(1, 1, token);
    auto res = map.get(1, token);
    std::cout << res->value << std::endl;
    int maxDepth;
    map.remove(1, token);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; i++) {
            threads.emplace_back([&map, i]() {
                doStuff(map, i);
            });
        }
    }
    for (int i = 0; i < size; i++) {
        if (auto ptr = map.get(i, token)) {
            std::cout << i << ": " << ptr->value << std::endl;
        } else {
            std::cout << i << ": null" << std::endl;
        }
    }
}

int main() {
    std::cout << (testMemoryReclaimation() ? "true" : "false") << "\n";
    return 0;
}