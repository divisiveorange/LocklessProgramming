This project is an attempt to make several lockless data structures in C++. 

Currently, it has a stack using a linked list (very basic as lockless programming goes) and a fixed size probing hashmap without reallocation of recycling tombstones.

My plan is to, based on my fixed size hashmap, make a resizable hashmap by concurrently maintaining 2 maps and incrementally moving elements, and then once that is done without freeing memory, implement Epoch Based Reclamation.

The code style of this project is the code I want to write for myself, so that the code executes as cleanly as possible despite the C++ code not neccessarily being too clean.

My biggest takeaway of this project is just that lockless programming is a fun challenge.
