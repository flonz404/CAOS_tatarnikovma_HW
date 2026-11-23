cmake_minimum_required(VERSION 3.16)
project(HW4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

foreach(role server client)
    add_executable(hotel_${role} ${role}.c hotel.c)
    target_compile_options(hotel_${role} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
