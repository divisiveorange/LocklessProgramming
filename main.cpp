#include <iostream>
#include <vector>
#include <__thread/jthread.h>

#include "FixedSizeHashMap.h"
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
    FixedSizeHashMap<int, std::string> map(10000);
    map.set(1, std::string("1"));
    auto res = map.get(1);
    map.remove(1);
    // Stack<int> stack;
    // {
    //     std::vector<std::jthread> threads;
    //     for (int i = 0; i < 15; i++) {
    //         threads.emplace_back([&stack]() {
    //             doStuff(stack);
    //         });
    //     }
    // }
    // while (auto node = stack.pop()) {
    //     std::cout << node->get() << std::endl;
    // }
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
    search both maps, larger first and return that value.
Deletion:
    search both and delete when found. Always search both
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