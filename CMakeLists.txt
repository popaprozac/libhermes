cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

# Hermes is C++; the binding implementation file is js.cc and needs
# CXX enabled at the project level.
project(js C CXX)

# Two embedding modes:
#
#   1. Embedded in Bare's build (the production path) — Bare's
#      top-level CMakeLists has already declared `uv`, `utf`, and
#      we just need to fetch Hermes ourselves.
#
#   2. Standalone (`cmake -B build` from this repo, for local
#      development of the binding) — nothing's declared, fetch
#      everything.
#
# `if(NOT TARGET …)` guards make each fetch opt-in.

# Hermes ships its source via facebook/hermes. Tags follow a
# date + RN version scheme (https://github.com/facebook/hermes/tags).
# Pin deliberately; bump on demand. We need Hermes' VM static lib
# (`hermesvm` historically; verify on first configure) and JSI
# headers from the same repo.
#
# CRITICAL: Hermes' CMakeLists.txt defaults BUILD_SHARED_LIBS to ON
# on non-Windows platforms and writes it to CMake's CACHE. When
# libhermes is fetched into Bare's build tree, that cache pollution
# poisons every subsequent fetch — most importantly BoringSSL (used
# by bare-tls / bare-crypto), which then builds as .dylib instead
# of .a, and the final zapp link can't find the static symbols.
#
# We pin BUILD_SHARED_LIBS=OFF (with FORCE so it wins over Hermes'
# `CACHE BOOL` default) BEFORE fetching Hermes, and also pin
# HERMES_BUILD_SHARED_JSI=OFF so libjsi builds as .a instead of
# .dylib.
#
# Do NOT use Hermes' `HERMES_STATIC_LINK=ON` switch — it appends
# `-static` to CMAKE_EXE_LINKER_FLAGS, which fails on macOS (no
# fully-static libc), and clamps CMAKE_FIND_LIBRARY_SUFFIXES to
# only `.a`, which breaks downstream macOS framework lookups.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Prefer producing static libs" FORCE)
set(HERMES_BUILD_SHARED_JSI OFF CACHE BOOL "Build JSI statically" FORCE)
if(NOT TARGET hermesvm)
  fetch_package("github:facebook/hermes@hermes-2025-09-01-RNv0.82.0-265ef62ff3eb7289d17e366664ac0da82303e101")
endif()

# libintrusive — Holepunch's intrusive data structures, used by every
# Bare engine binding for handle / scope lists. Always fetched.
fetch_package("github:holepunchto/libintrusive")

# libuv and libutf — fetched only in standalone mode. When Bare
# embeds us, it has already declared these targets and re-fetching
# would produce duplicate-target errors.
if(NOT TARGET uv)
  fetch_package("github:libuv/libuv@1.52.1")
endif()
if(NOT TARGET utf)
  fetch_package("github:holepunchto/libutf")
endif()

# We vendor `include/js.h` directly (from holepunchto/libjs) rather
# than fetch_package'ing libjs. cmake-fetch always add_subdirectory's
# the fetched repo, which would run libjs's CMakeLists — libjs is the
# V8 binding and requires GN to configure V8 itself. We only need the
# header, so vendoring sidesteps the gn dependency entirely.
#
# Bump `include/js.h` by re-curl'ing from holepunchto/libjs when the
# ABI rev's.

add_library(js OBJECT)

set_target_properties(
  js
  PROPERTIES
  CXX_STANDARD 17
  POSITION_INDEPENDENT_CODE ON
)

# js.cc is the Hermes binding implementation. C++ rather than C
# because Hermes' embedding API (JSI + hermes::vm) is C++; doing
# the translation from js.h's C ABI here keeps the C/C++ boundary
# at the binding edge.
target_sources(
  js
  PRIVATE
    src/js.cc
)

target_include_directories(
  js
  INTERFACE
    include
  PUBLIC
    include
  PRIVATE
    # Hermes target names are set by hermes' CMakeLists; the
    # canonical ones are `hermesvm` (the static lib) and `hermesvm`
    # for headers. Verify on first build and adjust if Hermes has
    # renamed since their cmake-bare integration.
    $<TARGET_PROPERTY:hermesvm,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:utf,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:intrusive,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(js_shared SHARED)

set_target_properties(
  js_shared
  PROPERTIES
  OUTPUT_NAME js
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(
  js_shared
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_shared
  PUBLIC
    hermesvm
    uv
    utf_shared
  PRIVATE
    js
)

add_library(js_static STATIC)

set_target_properties(
  js_static
  PROPERTIES
  OUTPUT_NAME js
  PREFIX lib
)

target_include_directories(
  js_static
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_static
  PUBLIC
    hermesvm
    uv_a
    utf_static
  PRIVATE
    js
)

install(TARGETS js_shared js_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
