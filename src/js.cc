// libhermes — Bare engine binding for Meta's Hermes.
//
// Implements the C ABI declared in `js.h` (sourced from
// holepunchto/libjs) on top of Hermes' embedding APIs. Same shape
// as libjsc / libqjs / libv8 / libmqjs; from Bare's perspective
// "Hermes" is just another `js_*` provider.
//
// This file is the integration point between two different ABIs:
//
//   - `js.h`   — C, ref-counted handle scopes, NAPI-shaped surface,
//                what Bare and every bare-* module's `binding.c`
//                calls into.
//   - Hermes VM — C++, JSI + hermes::vm::Runtime, lambda-based
//                callbacks, RAII managed values.
//
// The work of this file is essentially a translation table between
// the two. Other engine bindings in the Holepunch family are
// 2000-6000 LOC; expect similar here once it's fleshed out.
//
// === Build status ===
//
// SCAFFOLD ONLY. This source compiles to a stub library that
// reports "not implemented" for every js.h call. The intent is to
// get the CMakeLists + cmake-fetch wiring building cleanly first,
// THEN flesh out each js_* function one at a time. Milestone 1
// is `bare --eval 'console.log(1+1)'` working end-to-end, which
// needs roughly the following js.h surface:
//
//   js_create_platform / js_destroy_platform
//   js_create_env / js_destroy_env
//   js_open_handle_scope / js_close_handle_scope
//   js_get_global / js_set_named_property
//   js_create_string_utf8 / js_get_value_string_utf8
//   js_create_function / js_call_function
//   js_run_script
//   js_on_uncaught_exception
//
// Everything else (typed arrays, ArrayBuffers, Promises with
// libuv microtask integration, Object property descriptors, etc.)
// fills in incrementally as we hit each call from Bare's bootstrap.

extern "C" {
#include <js.h>
}

// Hermes embedding headers. Names may shift between Hermes versions
// — verify against the pinned `fetch_package("github:facebook/hermes@...")`
// in CMakeLists.txt. If Hermes' include layout has changed, update
// these paths and the `target_include_directories` for `hermesvm`
// in CMakeLists.
//
// Commented out until we have a real Hermes fetch wired so the
// scaffold compiles standalone. Uncomment once the first cmake
// configure succeeds and we know the actual include paths.
//
// #include <hermes/hermes.h>
// #include <hermes/Public/HermesExport.h>
// #include <jsi/jsi.h>

// Placeholder so the object file isn't empty. Replace with the
// first real `js_*` implementation when Phase 1 begins.
extern "C" int libhermes_stub_marker(void) {
  return 0;
}
