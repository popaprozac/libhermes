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

  // Variant 6: NAMED class expression where the class name shadows
  // the outer assignment target. This is the actual pattern in
  // Zapp's vite-bundled bare-events:
  //   t.exports = e = class e { constructor() {...} ... };
  //   e.EventEmitter = e;
  // The inner `class e` is a class expression; inside its body
  // the name `e` binds to the class itself. AFTER the assignment,
  // the OUTER `e` (a let/var binding) should hold the class.
  try_script(env, "named-class-shadow",
    "(function () {"
    "  let t = { exports: {} }, e;"
    "  t.exports = e = class e { constructor() { this.x = 1; } };"
    "  e.EventEmitter = e;"
    "  return typeof e + ',' + (e.EventEmitter === e);"
    "})()");

  // Variant 7: Same shadowing but with object property as the chain head.
  try_script(env, "named-class-prop-chain",
    "(function () {"
    "  let m = { exports: {} };"
    "  let exports = m.exports;"
    "  m.exports = exports = class exports { constructor() {} };"
    "  exports.EventEmitter = exports;"
    "  return typeof exports + ',' + (exports.EventEmitter === exports);"
    "})()");

  // Variant 8: PARAMETER as the outer binding — the exact production
  // shape. Zapp's worker.mjs has minified CommonJS wrappers like:
  //   (e, t, n, r) => { t.exports = e = class e {...}; e.EventEmitter = e; }
  // where `e` is the parameter (named `exports` before minification).
  try_script(env, "param-class-shadow",
    "(function () {"
    "  const fn = function (e, t) {"
    "    t.exports = e = class e { constructor() { this.x = 1; } };"
    "    e.EventEmitter = e;"
    "    return typeof e + ',' + (e.EventEmitter === e);"
    "  };"
    "  return fn({}, { exports: {} });"
    "})()");

  // Variant 9: Same as 8 but the wrapper is invoked via .call() with
  // a destructured/IIFE-style harness — closer to Vite's actual emit.
  try_script(env, "param-class-shadow-call",
    "(function () {"
    "  function load(fn) {"
    "    const m = { exports: {} };"
    "    fn(m.exports, m);"
    "    return m.exports;"
    "  }"
    "  const fn = function (e, t) {"
    "    t.exports = e = class e { constructor() {} };"
    "    e.EventEmitter = e;"
    "  };"
    "  load(fn);"
    "  return 'ok';"
    "})()");

  // Variant 10: EXACT production shape from Zapp's worker.mjs.
  // Module wrappers in the Vite bundle are ARROW functions with the
  // parameter named `e` (was `exports` pre-minification) shadowed by
  // a named class expression named `e`:
  //   o = (e, t) => () => (t || e((t={exports:{}}).exports, t), t.exports)
  //   l = o((e, t) => { t.exports = e = class e {...}; e.EventEmitter = e; })
  // Maybe Hermes' parser/binding differs for arrow functions
  // specifically.
  try_script(env, "arrow-param-class-shadow",
    "(function () {"
    "  const mod = ((e, t) => () => (t || e((t = { exports: {} }).exports, t), t.exports))"
    "    ((e, t) => {"
    "      t.exports = e = class e { constructor() { this.x = 1; } };"
    "      e.EventEmitter = e;"
    "    });"
    "  const r = mod();"
    "  return typeof r + ',' + (r.EventEmitter === r);"
    "})()");

  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));
  return 0;
}
