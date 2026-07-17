cmake_minimum_required(VERSION 3.15)
project(sieve_bufferpool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)  # -O2; the ops/sec numbers are meaningless without it
endif()

file(GLOB CORE_SRC src/*.cpp src/policies/*.cpp)
file(GLOB BENCH_SRC bench/*.cpp)

add_library(bufferpool ${CORE_SRC})
target_include_directories(bufferpool PUBLIC include)

add_executable(runner ${BENCH_SRC})
target_link_libraries(runner PRIVATE bufferpool)

add_executable(run_tests tests/run_tests.cpp)
target_link_libraries(run_tests PRIVATE bufferpool)
