cmake_minimum_required(VERSION 4.2)
project(LockLessProgramming)

set(CMAKE_CXX_STANDARD 26)

add_executable(LockLessProgramming main.cpp
        Stack.h
        Deque.h
        FixedSizeHashMap.h
        BasicStack.h
        ResizeableHashMap.h
        ThreadedArena.h
        ThreadedArena.h
        ThreadedArena.h)
