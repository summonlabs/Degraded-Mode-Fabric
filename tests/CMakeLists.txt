# Degraded Mode Fabric - test suites.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Every suite runs to completion. No test is given a timeout: a hang is a defect
# to diagnose, not a step to abandon.

add_library(dmf_testkit STATIC testkit/testkit.cpp testkit/process.cpp)
target_link_libraries(dmf_testkit PUBLIC dmf_runtime)
target_include_directories(dmf_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dmf_apply_warnings(dmf_testkit)
dmf_apply_sanitizers(dmf_testkit)

set(DMF_TEST_SUITES
  dmf_test_unit_domain
  dmf_test_protocol_codec
  dmf_test_persistence
  dmf_test_integration
  dmf_test_property_engine
  dmf_test_concurrency
  dmf_test_multiprocess
  dmf_test_scale
)

foreach(suite IN LISTS DMF_TEST_SUITES)
  add_executable(${suite} ${suite}.cpp)
  target_link_libraries(${suite} PRIVATE dmf_testkit)
  dmf_apply_warnings(${suite})
  dmf_apply_sanitizers(${suite})
  add_test(NAME ${suite} COMMAND ${suite})
endforeach()

# The multiprocess suite drives real tool processes built next to it and locates
# them relative to its own executable, so no absolute build path is baked in.
if(TARGET dmf_coordinator)
  add_dependencies(dmf_test_multiprocess dmf_coordinator dmf_publisher dmf_agent dmf_cli)
endif()
