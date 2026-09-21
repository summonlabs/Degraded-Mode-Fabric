cmake_minimum_required(VERSION 3.20)

project(DegradedModeFabric
  VERSION 1.0.0
  DESCRIPTION "Degraded Mode Fabric - explicit degraded-mode policy and authority runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Degraded-Mode-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(DMF_BUILD_TESTS "Build the Degraded Mode Fabric test suites" ON)
option(DMF_BUILD_TOOLS "Build the Degraded Mode Fabric command line tools" ON)
option(DMF_BUILD_EXAMPLES "Build the Degraded Mode Fabric examples" ON)
option(DMF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(DMF_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(DMF_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(DMF_DISCOVER_SANITIZER_SUPPORT "Probe the toolchain for sanitizer support" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(DmfWarnings)
include(DmfSanitizers)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(DMF_CORE_SOURCES
  src/core/core.cpp
  src/core/ids.cpp
  src/core/reason.cpp
  src/domain/guarantee.cpp
  src/domain/contract.cpp
  src/domain/evidence.cpp
  src/domain/authority.cpp
  src/domain/policy.cpp
  src/domain/decision.cpp
  src/domain/grant.cpp
  src/codec/codec.cpp
  src/engine/evaluator.cpp
  src/engine/allocator.cpp
  src/engine/reference_solver.cpp
  src/engine/validator.cpp
  src/engine/restoration.cpp
)

add_library(dmf_core STATIC ${DMF_CORE_SOURCES})
add_library(DegradedModeFabric::dmf_core ALIAS dmf_core)
target_include_directories(dmf_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(dmf_core PROPERTIES OUTPUT_NAME dmf_core)
dmf_apply_warnings(dmf_core)
dmf_apply_sanitizers(dmf_core)

set(DMF_RUNTIME_SOURCES
  src/store/file_io.cpp
  src/store/journal.cpp
  src/store/snapshot.cpp
  src/store/state_store.cpp
  src/net/frame.cpp
  src/net/socket.cpp
  src/net/messages.cpp
  src/net/server.cpp
  src/net/client.cpp
  src/runtime/accounting.cpp
  src/runtime/evidence_store.cpp
  src/runtime/grant_table.cpp
  src/runtime/coordinator.cpp
)

add_library(dmf_runtime STATIC ${DMF_RUNTIME_SOURCES})
add_library(DegradedModeFabric::dmf_runtime ALIAS dmf_runtime)
target_include_directories(dmf_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Private headers (src/store/*.hpp) are implementation detail and are never
# installed.
target_include_directories(dmf_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dmf_runtime PUBLIC dmf_core Threads::Threads)
if(WIN32)
  target_link_libraries(dmf_runtime PUBLIC ws2_32)
endif()
set_target_properties(dmf_runtime PROPERTIES OUTPUT_NAME dmf_runtime)
dmf_apply_warnings(dmf_runtime)
dmf_apply_sanitizers(dmf_runtime)

add_library(DegradedModeFabric::dmf ALIAS dmf_runtime)

if(DMF_BUILD_TOOLS)
  set(DMF_TOOLS dmf_coordinator dmf_publisher dmf_agent dmf_cli dmf_selftest)
  foreach(tool IN LISTS DMF_TOOLS)
    add_executable(${tool} tools/${tool}_main.cpp)
    target_link_libraries(${tool} PRIVATE dmf_runtime)
    dmf_apply_warnings(${tool})
    dmf_apply_sanitizers(${tool})
  endforeach()
  install(TARGETS ${DMF_TOOLS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(DMF_BUILD_EXAMPLES)
  add_executable(dmf_example_degradation examples/degradation_walkthrough.cpp)
  target_link_libraries(dmf_example_degradation PRIVATE dmf_runtime)
  dmf_apply_warnings(dmf_example_degradation)
  dmf_apply_sanitizers(dmf_example_degradation)
endif()

if(DMF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS dmf_core dmf_runtime
  EXPORT DegradedModeFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT DegradedModeFabricTargets
  FILE DegradedModeFabricTargets.cmake
  NAMESPACE DegradedModeFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedModeFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DegradedModeFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedModeFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedModeFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedModeFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedModeFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedModeFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedModeFabric)
