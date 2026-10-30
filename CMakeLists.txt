# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)
project(open_carshare C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(OCS_SANITIZE "Build with ASan and UBSan" ON)
if(OCS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

option(OCS_FUZZ "Build libFuzzer targets (clang only)" OFF)
if(OCS_FUZZ AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "OCS_FUZZ requires clang (CC=clang)")
endif()

enable_testing()
add_subdirectory(server)
