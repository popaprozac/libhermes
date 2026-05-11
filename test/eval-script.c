// Smoke test: evaluate `'hello, hermes'` via js_run_script and read
// the result back through js_get_value_string_utf8. Validates the
// minimal Phase 1 path:
//   js_create_platform → js_create_env → js_open_handle_scope
//   → js_create_string_utf8 (the source code itself)
//   → js_run_script
//   → js_get_value_string_utf8 (the result)
//   → js_close_handle_scope → js_destroy_env → js_destroy_platform
//
// Once this passes, `bare --eval` is one step away — we just need
// js_create_function + console.log host binding to print from inside
// the script rather than reading the result back out.

// IMPORTANT: don't wrap call expressions in `assert(...)` here —
// CMake's Release config defines NDEBUG, which compiles assertions
// to no-ops. That silently swallows the calls themselves and the
// test runs through a series of phantom side-effects. Use a `CHECK`
// macro that survives optimization.
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[eval-script] FAIL: %s -> %d\n", #expr, _rc); \
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

  // Wrap in IIFE because JSI's evaluateJavaScript discards the
  // script's "completion value" semantics — only an explicit
  // function-call expression result is captured. The IIFE makes
  // the value return path explicit.
  const char *code = "(function () { return 'hello, hermes'; })()";
  js_value_t *source;
  CHECK(js_create_string_utf8(env, (const utf8_t *) code, strlen(code), &source));

  js_value_t *result;
  CHECK(js_run_script(env, "test.js", strlen("test.js"), 0, source, &result));

  size_t len = 0;
  CHECK(js_get_value_string_utf8(env, result, NULL, 0, &len));

  char buf[64];
  CHECK(js_get_value_string_utf8(env, result, (utf8_t *) buf, sizeof(buf) - 1, &len));
  buf[len] = '\0';

  printf("[eval-script] result: '%s' (len=%zu)\n", buf, len);
  if (strcmp(buf, "hello, hermes") != 0) {
    fprintf(stderr, "[eval-script] FAIL: expected 'hello, hermes', got '%s'\n", buf);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[eval-script] OK\n");
  return 0;
}
