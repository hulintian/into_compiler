cmake_minimum_required(VERSION 3.22.1)

project(kaleidoscope)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

set(CMAKE_BUILD_TYPE "Debug")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb") # 定义Debug编译参数
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall") # 定义Release编译参数

set(CMAKE_CXX_STANDARD 14 CACHE STRING "")
# set(CMAKE_VERBOSE_MAKEFILE ON)

set(LT_LLVM_INSTALL_DIR "" CACHE PATH "/usr/local/llvm-13")
set(LT_LLVM_CMAKE_CONFIG_DIR "${LT_LLVM_INSTALL_DIR}/lib/cmake/llvm")
set(LT_CLANG_CMAKE_CONFIG_DIR "${LT_LLVM_INSTALL_DIR}/lib/cmake/clang")

# set compiler
#set(CMAKE_CXX_COMPILER "/usr/local/llvm-13/bin/clang++")
# set(CMAKE_C_COMPILER "usr/local/llvm-13/bin/clang)

set(search_paths "/usr/local/llvm-13/lib/cmake/clang")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "clang search path: ${search_paths}")

list(APPEND CMAKE_PREFIX_PATH "${LT_LLVM_CMAKE_CONFIG_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${LT_CLANG_CMAKE_CONFIG_DIR}")

find_package(LLVM 13.0.0 PATHS "/usr/local/llvm-13/lib/cmake/llvm" REQUIRED CONFIG NO_DEFAULT_PATH)
find_package(Clang PATHS "/usr/local/llvm-13/lib/cmake/clang" CONFIG NO_DEFAULT_PATH )

link_directories("/usr/local/llvm-13/lib")

message("cmake module path: " ${CMAKE_MODULE_PATH})
message(STATUS "Found llvm ${LLVM_PACKAGE_VERSION}")
message(STATUS "Found clang ${CLANG_CMAKE_DIR}")
message(STATUS "Using LLVMConfig.cmake in ${LLVM_DIR}")
message(STATUS "CPP standard is ${CMAKE_CXX_STANDARD}")
message(STATUS "include lib c++ : ${CXX_STD_LIB}")

include_directories(${LLVM_INCLUDE_DIRS})

message(STATUS "LLVM include dirs is ${LLVM_INCLUDE_DIRS}")

set(CMAKE_SOURCE_DIR "src/")
set(CMAKE_MODULE_PATH "../bin")

# config include libraries, headers, flags
include(CheckIncludeFile)
include(CheckIncludeFileCXX)
include(CheckLibraryExists)
include(CheckCXXCompilerFlag)

# add LLVM configuration
list(APPEND CMAKE_MODULE_PATH ${LLVM_DIR})

# Use LLVM
add_definitions(${LLVM_DIFINITIONS})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
llvm_map_components_to_libnames(llvm_libs Core)

if(NOT LLVM_ENABLE_RTTI)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

add_subdirectory("src")