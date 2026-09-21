# Degraded Mode Fabric - independent downstream consumer.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# This project is deliberately separate from the DMF source tree: it consumes
# only the installed package, through find_package(... CONFIG REQUIRED), and
# never sees a DMF source file.
cmake_minimum_required(VERSION 3.20)
project(DmfConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(DegradedModeFabric 1.0 CONFIG REQUIRED)

add_executable(dmf_consumer main.cpp)
target_link_libraries(dmf_consumer PRIVATE DegradedModeFabric::dmf_runtime)
if(MSVC)
  target_compile_options(dmf_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(dmf_consumer PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()
