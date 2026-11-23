cmake_minimum_required(VERSION 3.16)
project(HW2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
add_executable(hotel hotel.c)
target_link_libraries(hotel PRIVATE Threads::Threads)
target_compile_options(hotel PRIVATE -Wall -Wextra -Wpedantic)
