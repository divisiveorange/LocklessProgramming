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
                        auto result = map.remove(size*i + j, token);
                        if (!result) {

                        }
                        if (map.get(size*i+j, token)) {
                                std::cout << size*i+j << " (just removed) wasn't removed\n";
                                std::this_thread::sleep_for(std::chrono::nanoseconds(1000000));
                                if (map.get(size*i+j, token)) {
                                    std::cout << "Get is correct, remove is wrong. Remove reports " << (result) << std::endl;
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
    std::cout << (testDataRemains() ? "true" : "false") << "\n";
    return 0;
}




/*
    Future ideas and plans:
        a non-resizable hashmap:
            A flatmap.
            An array of Payload*, nullptr for empty, 1 for tombstone and anything else for a value
            No tombstone reusing
            Inserting goes to the hash and then probes until it finds the value.
            If the value or empty are found, it CASs to avoid a tombstone. If it is a tombstone it moves to the next node.
            Searching just goes to the hash and probes until it finds the value or empty.
            Deleting is searching but turns it to a tombstone if found.

        Resizeable hashmap:
2 flatmaps. A small one and a large one. They should have pointers to payloads (key + value).
Searching:
    search both maps, larger first, smaller and then larger again and return that value.
Deletion:
    search both and delete when found. Always search both. If a moved node is found, move to larger with a tombstone
    with the same pointer.
Insertion:
    insert into larger, then if there is a value in smaller, remove it, else move some other value from
    smaller to larger by copying it to larger, then removing from smaller.
    If moved item already exists in the large map, remove it from the small map and then move another item

Before inserting an item, a size variable should be incremented, and then decremented if no new node was created.
After removing an item, decrement the size variable.
When the size of the small map is zero (only 1 thread gets to this point),
make the large array the small array and allocate an array twice the size.

If the large map runs out of space before the small map has been cleared because many threads were paused
before removing items from the small map, just throw an exception or use a mutex
(given a futex is basically free to check if inactive).


*/