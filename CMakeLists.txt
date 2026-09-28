cmake_minimum_required(VERSION 3.20)
project(c2d LANGUAGES CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# c2d -- native ELF/DWARF dumper
#
# Module layout. Each layer depends only on the ones above it, so a change to
# ELF parsing cannot ripple into type reconstruction or output.
#   util/   : byte cursors, LEB128, endianness, small helpers
#   diag/   : logging, metrics/timers, process resource reporting
#   elf/    : ELF container parsing only (no DWARF knowledge)
#   dwarf/  : DWARF abbrev/unit/DIE decoding only (no type semantics)
#   ir/     : type system intermediate representation
#   output/ : C# source generation
#   app/    : CLI front-end
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(C2D_BUILD_TESTS "Build the c2d test suite" ON)
option(C2D_WERROR "Treat warnings as errors" OFF)

# Default to -O2 even in "Debug" style builds: the hot paths (DIE walking) are
# unusably slow at -O0 for a 200 MB .debug_info, and tests are always built in
# Release-like mode here.
# Warning and optimisation flags are compiler-specific: MSVC spells them
# differently and does not accept -Wall at all.
if(MSVC)
  add_library(c2d_flags INTERFACE)
  target_compile_options(c2d_flags INTERFACE /W4 /permissive-)
  target_compile_definitions(c2d_flags INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
  if(C2D_WERROR)
    target_compile_options(c2d_flags INTERFACE /WX)
  endif()
else()
  add_library(c2d_flags INTERFACE)
  target_compile_options(c2d_flags INTERFACE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-omit-frame-pointer>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<AND:$<BOOL:${C2D_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
  )
endif()

# psapi is needed for the Windows memory counters in diag/metrics.
if(WIN32)
  target_link_libraries(c2d PUBLIC psapi)
endif()

add_library(c2d STATIC
  src/util/bytes.cpp
  src/diag/log.cpp
  src/diag/metrics.cpp
  src/elf/mapped_file.cpp
  src/elf/elf_file.cpp
  src/elf/phdr_recovery.cpp
  src/dwarf/sections.cpp
  src/dwarf/abbrev.cpp
  src/dwarf/unit.cpp
  src/dwarf/constants.cpp
  src/dwarf/eh_frame.cpp
  src/dwarf/dwarf_context.cpp
  src/ir/model.cpp
  src/ir/build.cpp
  src/output/emit_il2cpp.cpp
)
target_include_directories(c2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(c2d PUBLIC c2d_flags)

add_executable(c2d-dump src/app/main.cpp)
target_link_libraries(c2d-dump PRIVATE c2d)
set_target_properties(c2d-dump PROPERTIES OUTPUT_NAME c2d)

if(C2D_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "c2d: build type=${CMAKE_BUILD_TYPE} tests=${C2D_BUILD_TESTS}")
