cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

# Hermes is C++; the binding implementation file is js.cc and needs
# CXX enabled at the project level.
project(js C CXX)

# Hermes ships its source via facebook/hermes. Pin to a specific
# release tag so builds are reproducible — bump deliberately. The
# Hermes commit chosen here should be vetted against:
#   - LLVM/CMake compat (Hermes carries vendored LLVM utilities)
#   - iOS cross-build (CMAKE_MACOSX_BUNDLE=OFF must be honored by
#     the build, same trick as our BoringSSL integration in zapp)
#   - ES feature coverage Zapp depends on (regex, Intl subset)
#
# TODO: pin actual commit / tag here before first real build.
fetch_package("github:facebook/hermes@hermes-v0.13.0")

# Pull libjs (the js.h header) from Holepunch — same source-of-truth
# for the engine ABI that libjsc / libqjs / libv8 all bind against.
# We DON'T link libjs.a; just need its include/ directory.
fetch_package("github:holepunchto/libjs" SOURCE_DIR js)
fetch_package("github:holepunchto/libintrusive")

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
    ${js}/include
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
