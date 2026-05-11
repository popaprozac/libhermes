// Wrap/unwrap: attach a C struct to a JS object, fish it out later.
// This is how bare-tcp/bare-tls etc. associate native handles with
// JS instances — JS sees a plain object; C-side code uses unwrap
// to recover the (uv_tcp_t*, etc.) pointer it stashed.
//
// Test: simulate a "counter" struct that holds a counter value.
// Create a JS object, wrap it with a pointer to our counter, run
// JS that calls a method which pulls the wrapped state out and
// increments it, then verify from C.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[wrap-unwrap] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

typedef struct {
  int counter;
} CounterState;

// Bumps the wrapped counter for the receiving object. JS calls
// like: counterObj.bump()
static js_value_t *
bump(js_env_t *env, js_callback_info_t *info) {
  js_value_t *receiver;
  CHECK(js_get_callback_info(env, info, NULL, NULL, &receiver, NULL));

  void *data;
  CHECK(js_unwrap(env, receiver, &data));
  CounterState *state = (CounterState *) data;
  state->counter++;

  js_value_t *out;
  CHECK(js_create_int32(env, state->counter, &out));
  return out;
}

int
main(void) {
  uv_loop_t *loop = uv_default_loop();
  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));
  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  // Build a JS object; wrap with a heap-allocated counter.
  CounterState *state = (CounterState *) calloc(1, sizeof(*state));
  state->counter = 10;

  js_value_t *obj;
  CHECK(js_create_object(env, &obj));
  CHECK(js_wrap(env, obj, state, NULL, NULL, NULL));

  // Attach `bump` method.
  js_value_t *bump_fn;
  CHECK(js_create_function(env, "bump", (size_t) -1, bump, NULL, &bump_fn));
  CHECK(js_set_named_property(env, obj, "bump", bump_fn));

  // Make it global so the script can reach it.
  js_value_t *global;
  CHECK(js_get_global(env, &global));
  CHECK(js_set_named_property(env, global, "counter", obj));

  // Run a script that calls bump() three times; expect 13.
  const char *src = "(function () { counter.bump(); counter.bump(); return counter.bump(); })()";
  js_value_t *src_v, *result;
  CHECK(js_create_string_utf8(env, (const utf8_t *) src, strlen(src), &src_v));
  CHECK(js_run_script(env, "t.js", strlen("t.js"), 0, src_v, &result));

  int32_t final = 0;
  CHECK(js_get_value_int32(env, result, &final));
  printf("[wrap-unwrap] final = %d, state->counter = %d\n", final, state->counter);

  if (final != 13 || state->counter != 13) {
    fprintf(stderr, "[wrap-unwrap] FAIL: expected 13/13, got %d/%d\n", final, state->counter);
    return 1;
  }

  // Round-trip through unwrap directly from C.
  void *recovered = NULL;
  CHECK(js_unwrap(env, obj, &recovered));
  if (recovered != state) {
    fprintf(stderr, "[wrap-unwrap] FAIL: unwrap recovered %p, expected %p\n",
      recovered, (void *) state);
    return 1;
  }

  free(state);

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[wrap-unwrap] OK\n");
  return 0;
}
