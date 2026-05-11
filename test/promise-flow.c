// Promise flow:
//
// 1. C creates a promise via js_create_promise (returns deferred + promise).
// 2. C resolves it from the host side via js_resolve_deferred.
// 3. JS sees the value through `.then` and writes it to a slot we
//    can read back.
//
// The wrinkle: Hermes' .then callbacks queue microtasks. We have to
// explicitly drain them after resolving — otherwise the .then handler
// hasn't fired yet when we check the result slot.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

// libhermes extension: drain pending Hermes microtasks. Not in
// js.h yet — Bare's libuv tick will call this automatically once
// the engines are wired in. Forward-declare locally for the test.
extern int js_run_microtasks(js_env_t *env);

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[promise-flow] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

int
main(void) {
  uv_loop_t *loop = uv_default_loop();
  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));
  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  // Make the promise + its deferred.
  js_deferred_t *deferred;
  js_value_t *promise;
  CHECK(js_create_promise(env, &deferred, &promise));

  bool is_p = false;
  CHECK(js_is_promise(env, promise, &is_p));
  if (!is_p) { fprintf(stderr, "[promise-flow] FAIL: is_promise false\n"); return 1; }

  // Attach a .then that stashes the resolved value in a global slot.
  js_value_t *global;
  CHECK(js_get_global(env, &global));
  CHECK(js_set_named_property(env, global, "__p", promise));

  const char *setupSrc =
    "globalThis.__got = null;"
    "__p.then(v => { globalThis.__got = v; });";
  js_value_t *setup;
  CHECK(js_create_string_utf8(env, (const utf8_t *) setupSrc, strlen(setupSrc), &setup));
  CHECK(js_run_script(env, "setup.js", strlen("setup.js"), 0, setup, NULL));

  // Resolve from C side with 42.
  js_value_t *val;
  CHECK(js_create_int32(env, 42, &val));
  CHECK(js_resolve_deferred(env, deferred, val));

  // Drain pending microtasks so the .then handler actually runs.
  // Until libuv tick integration lands, callers do this explicitly.
  js_run_microtasks(env);

  // Read the slot the .then handler wrote.
  const char *readSrc = "globalThis.__got";
  js_value_t *readScript;
  CHECK(js_create_string_utf8(env, (const utf8_t *) readSrc, strlen(readSrc), &readScript));
  js_value_t *got;
  CHECK(js_run_script(env, "read.js", strlen("read.js"), 0, readScript, &got));

  bool is_num = false;
  CHECK(js_is_number(env, got, &is_num));
  if (!is_num) {
    js_value_type_t k;
    CHECK(js_typeof(env, got, &k));
    fprintf(stderr, "[promise-flow] FAIL: expected number, typeof=%d\n", k);
    return 1;
  }
  int32_t n = 0;
  CHECK(js_get_value_int32(env, got, &n));
  printf("[promise-flow] resolved value = %d (expected 42)\n", n);
  if (n != 42) { fprintf(stderr, "[promise-flow] FAIL: got %d\n", n); return 1; }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[promise-flow] OK\n");
  return 0;
}
