# Degraded Mode Fabric - strict compiler warning configuration.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

function(dmf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /EHsc
      /MP
    )
    if(DMF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wold-style-cast
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
    )
    if(DMF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
