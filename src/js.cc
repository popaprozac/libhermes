// libhermes — Bare engine binding for Meta's Hermes.
//
// Implements the C ABI declared in `js.h` (sourced from
// holepunchto/libjs) on top of Hermes' embedding APIs. Same shape
// as libjsc / libqjs / libv8 / libmqjs; from Bare's perspective
// "Hermes" is just another `js_*` provider.
//
// === Translation between two ABIs ===
//
//   js.h          C, ref-counted handle scopes, NAPI-shaped surface,
//                 what Bare and every bare-* module's `binding.c`
//                 calls into.
//   Hermes VM     C++, JSI + hermes::vm::Runtime, lambda-based
//                 callbacks, RAII-managed values, persistent
//                 references via jsi::Object's PropNameID/Function/
//                 etc. wrappers.
//
// The bulk of this file is a translation table. By analogy with
// libqjs (~6.5K LOC) expect a similar size when complete.
//
// === Phase 1 milestone ===
//
// `bare --eval 'console.log(1+1)'` end-to-end. That requires:
//
//   ✗ js_create_platform / js_destroy_platform
//   ✗ js_create_env / js_destroy_env
//   ✗ js_open_handle_scope / js_close_handle_scope
//   ✗ js_get_global / js_set_named_property
//   ✗ js_create_string_utf8 / js_get_value_string_utf8
//   ✗ js_create_function / js_call_function
//   ✗ js_run_script
//   ✗ js_on_uncaught_exception
//
// Functions below are currently STUBS that return `-1` for "not
// implemented". Implement one at a time; flip the checkbox above
// when each lands.

extern "C" {
#include <js.h>
}

#include <hermes/hermes.h>
#include <jsi/jsi.h>

#include <memory>
#include <string>
#include <vector>

namespace jsi = ::facebook::jsi;
using HermesRuntime = ::facebook::hermes::HermesRuntime;

// === Opaque type definitions =========================================
//
// `js.h` declares these as forward-only `struct X;` so consumers
// can hold pointers but not poke at internals. We define the actual
// layouts here. Field names are tentative — adjust as we add APIs.

struct js_platform_s {
  uv_loop_t *loop;
  // Hermes is more lightweight than V8 — no separate "platform"
  // object holding worker threads / ICU / etc. The js_platform_t
  // exists mainly to give Bare a thing to hand out and to hold the
  // event loop. Per-VM state lives on js_env_t.
};

struct js_env_s {
  uv_loop_t *loop;
  js_platform_t *platform;
  std::unique_ptr<HermesRuntime> runtime;

  // Handle scopes form a stack. Each scope owns the values that
  // were materialized while it was open; closing a scope releases
  // them. We model scopes as a linked list per env; js_value_t*
  // pointers reference entries inside.
  js_handle_scope_t *top_scope = nullptr;

  // Set by js_on_uncaught_exception; called from JSI's
  // ::setThrowFromHostFunction hook (Hermes routes thrown
  // exceptions through that — TODO confirm exact API on first use).
  js_uncaught_exception_cb on_uncaught = nullptr;
  void *on_uncaught_data = nullptr;
};

// A scope owns a vector of values; closing the scope drops them all.
// `prev` chains up the stack so `js_close_handle_scope` can pop.
struct js_handle_scope_s {
  js_env_t *env;
  js_handle_scope_t *prev;
  std::vector<std::unique_ptr<js_value_s>> values;
};

// `js_value_t` is one node in a scope's owned-value list. Holds a
// JSI Value (which is itself a tagged union — Number / String /
// Object / etc. — internally; jsi::Value handles the dispatch).
//
// NOTE: jsi::Value is move-only and non-copyable. Wrap in a struct
// so the C API can pass `js_value_t*` around without copies.
struct js_value_s {
  jsi::Value value;
  // Reference to the runtime that owns this value's GC root. Mostly
  // bookkeeping today; matters when we wire persistent references.
  HermesRuntime *runtime;
};

// === Stub macros =====================================================
//
// Until each function is implemented, return -1 to make missing
// functionality obvious at runtime rather than silently returning
// uninitialized memory.

#define NOT_IMPL(fn) \
  do {               \
    (void) fn;       \
    return -1;       \
  } while (0)

// === Platform / Env =================================================

extern "C" int
js_create_platform(uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  (void) options;
  auto *p = new js_platform_s{loop};
  *result = p;
  return 0;
}

extern "C" int
js_destroy_platform(js_platform_t *platform) {
  delete platform;
  return 0;
}

extern "C" int
js_get_platform_loop(js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;
  return 0;
}

extern "C" int
js_get_platform_identifier(js_platform_t *platform, const char **result) {
  (void) platform;
  *result = "hermes";
  return 0;
}

extern "C" int
js_get_platform_version(js_platform_t *platform, const char **result) {
  (void) platform;
  // TODO: return Hermes' actual version string from
  // HermesRuntime::getVersion() or similar API.
  *result = "0.0.0-libhermes-scaffold";
  return 0;
}

extern "C" int
js_create_env(uv_loop_t *loop, js_platform_t *platform, const js_env_options_t *options, js_env_t **result) {
  (void) options;
  auto *env = new js_env_s();
  env->loop = loop;
  env->platform = platform;
  env->runtime = ::facebook::hermes::makeHermesRuntime();
  *result = env;
  return 0;
}

extern "C" int
js_destroy_env(js_env_t *env) {
  delete env;
  return 0;
}

extern "C" int
js_get_env_loop(js_env_t *env, uv_loop_t **result) {
  *result = env->loop;
  return 0;
}

extern "C" int
js_get_env_platform(js_env_t *env, js_platform_t **result) {
  *result = env->platform;
  return 0;
}

extern "C" int
js_on_uncaught_exception(js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->on_uncaught = cb;
  env->on_uncaught_data = data;
  return 0;
}

// === Handle scopes ==================================================

extern "C" int
js_open_handle_scope(js_env_t *env, js_handle_scope_t **result) {
  auto *scope = new js_handle_scope_s();
  scope->env = env;
  scope->prev = env->top_scope;
  env->top_scope = scope;
  *result = scope;
  return 0;
}

extern "C" int
js_close_handle_scope(js_env_t *env, js_handle_scope_t *scope) {
  if (env->top_scope != scope) {
    // Out-of-order scope close is a programming error in caller.
    // libqjs / libjsc abort here; we'd do the same. For now return
    // an error code.
    return -1;
  }
  env->top_scope = scope->prev;
  delete scope; // releases all owned js_value_s
  return 0;
}

// === Stubs for the rest of Phase 1 ==================================
//
// Sketch the function bodies enough to compile; implement when each
// becomes the next blocker. All return -1 until then.

extern "C" int
js_create_string_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  (void) env; (void) str; (void) len; (void) result;
  NOT_IMPL("js_create_string_utf8");
}

extern "C" int
js_get_value_string_utf8(js_env_t *env, js_value_t *value, utf8_t *buf, size_t len, size_t *result) {
  (void) env; (void) value; (void) buf; (void) len; (void) result;
  NOT_IMPL("js_get_value_string_utf8");
}

extern "C" int
js_get_global(js_env_t *env, js_value_t **result) {
  (void) env; (void) result;
  NOT_IMPL("js_get_global");
}

extern "C" int
js_set_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  (void) env; (void) object; (void) name; (void) value;
  NOT_IMPL("js_set_named_property");
}

extern "C" int
js_create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  (void) env; (void) name; (void) len; (void) cb; (void) data; (void) result;
  NOT_IMPL("js_create_function");
}

extern "C" int
js_call_function(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  (void) env; (void) receiver; (void) function; (void) argc; (void) argv; (void) result;
  NOT_IMPL("js_call_function");
}

extern "C" int
js_run_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_value_t **result) {
  (void) env; (void) file; (void) len; (void) offset; (void) source; (void) result;
  NOT_IMPL("js_run_script");
}
