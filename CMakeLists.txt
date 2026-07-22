cmake_minimum_required(VERSION 3.10)
project(tspv_packet_processor C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

add_library(tspv_processor STATIC
    src/tspv_processor.c
)
target_include_directories(tspv_processor PUBLIC include)

enable_testing()
add_executable(tspv_tests tests/test_tspv_processor.c)
target_link_libraries(tspv_tests PRIVATE tspv_processor)
add_test(NAME tspv_tests COMMAND tspv_tests)
