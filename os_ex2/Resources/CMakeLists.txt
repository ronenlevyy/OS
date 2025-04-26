cmake_minimum_required(VERSION 3.30)
project(Resources)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(uthreads uthreads.cpp)
add_executable(test uthreads.h uthreads.cpp tests/test6.cpp)