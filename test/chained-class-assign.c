// Reproducer: bare-events does the CommonJS pattern
//
//   module.exports = exports = class EventEmitter {...}
//   exports.EventEmitter = exports
//
// When bare-hermes runs the bundled bare.js inside the bare runtime,
// the second line throws "Cannot set property 'EventEmitter' of
// undefined". This test isolates that pattern.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[chained-class-assign] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static void
try_script(js_env_t *env, const char *label, const char *code) {
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  js_value_t *source;
  CHECK(js_create_string_utf8(env, (const utf8_t *) code, strlen(code), &source));

  js_value_t *result;
  int rc = js_run_script(env, label, strlen(label), 0, source, &result);
  if (rc != 0) {
    fprintf(stderr, "[%s] js_run_script returned %d\n", label, rc);
  } else {
    size_t len = 0;
    js_get_value_string_utf8(env, result, NULL, 0, &len);
    char buf[256];
    js_get_value_string_utf8(env, result, (utf8_t *) buf, sizeof(buf), &len);
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    fprintf(stderr, "[%s] result: %s\n", label, buf);
  }

  CHECK(js_close_handle_scope(env, scope));
}

int
main(void) {
  uv_loop_t *loop = uv_default_loop();

  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));

  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));

  // Variant 1: bare arrow function (no chained assignment, no class).
  try_script(env, "arrow-only",
    "(function () { const fn = (a, b) => a + b; return String(fn(1, 2)); })()");

  // Variant 2: plain class expression assignment.
  try_script(env, "class-expr",
    "(function () { const X = class Foo {}; return typeof X; })()");

  // Variant 3: chained assignment with a simple value.
  try_script(env, "chained-simple",
    "(function () { let a, b; a = b = 42; return a + ',' + b; })()");

  // Variant 4: chained assignment with parenthesized class expression.
  try_script(env, "chained-paren-class",
    "(function () { let a, b; a = b = (class Foo {}); return typeof b; })()");

  // Variant 5: chained assignment with bare class expression — the
  // exact pattern from bare-events line 455.
  try_script(env, "chained-bare-class",
    "(function () { let a, b; a = b = class Foo {}; return typeof b; })()");

  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));
  return 0;
}
