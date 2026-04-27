cmake_minimum_required(VERSION 3.18)
project(cs6245-dswp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM 15 REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake from: ${LLVM_DIR}")
message(STATUS "LLVM version: ${LLVM_PACKAGE_VERSION}")

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

add_subdirectory(pass)
add_subdirectory(runtime)
add_subdirectory(benchmarks)
