// Reproducer: the minified CommonJS-interop loader pattern that
// Vite emits at the top of every Zapp worker bundle, e.g.
//
//   o = (e, t) => () => (
//     t || e((t={exports:{}}).exports, t),
//     t.exports
//   )
//
// `o` wraps each CJS module's factory function. The inner closure:
//   - lazy-evaluates the factory once (`t` starts undefined, so
//     `t || ...` runs the RHS the first time, then on subsequent
//     calls returns the cached `t.exports`)
//   - assigns `t` inline as a side effect of the first argument
//     (`(t = {exports: {}}).exports`) so the closure's outer-scope
//     `t` is now the module record. The second arg references
//     that same closure variable AFTER the assignment side effect.
//
// Inside the module factory `(exports, module) => { ... }`, the
// first param `exports` should be the `{}` object. In real bare-
// hermes runs we see exports come through as `undefined`, which
// means somewhere along this exact chain Hermes is not honoring
// either left-to-right arg evaluation OR the closure variable
// reassignment from an arg expression.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[cjs-loader-pattern] FAIL: %s -> %d\n", #expr, _rc); \
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
    char buf[512];
    size_t cap = sizeof(buf) - 1;
    js_get_value_string_utf8(env, result, (utf8_t *) buf, cap, &len);
    buf[len < cap ? len : cap] = '\0';
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

  // Test 1: Does Hermes honor left-to-right arg eval with side effects?
  //   foo(a={1}, a) — second arg should be the new a (= 1), not the old.
  try_script(env, "side-effect-args",
    "(function () {\n"
    "  let log = '';\n"
    "  const sink = (a, b) => log += 'a=' + typeof a + ',b=' + typeof b;\n"
    "  let t;\n"
    "  sink((t = {x: 1}).x, t);\n"
    "  return log;\n"
    "})()");

  // Test 2: Reassign a closure variable from inside a call arg.
  try_script(env, "closure-reassign-via-arg",
    "(function () {\n"
    "  const f = (e, t) => () => (t || e((t={exports:{}}).exports, t), t.exports);\n"
    "  const factory = (exports, module) => { exports.foo = 'bar'; };\n"
    "  const wrapped = f(factory);\n"
    "  const result = wrapped();\n"
    "  return JSON.stringify(result);\n"
    "})()");

  // Test 3: The full Vite CJS-interop pattern with a class export.
  try_script(env, "vite-cjs-class",
    "(function () {\n"
    "  const o = (e, t) => () => (t || e((t = {exports: {}}).exports, t), t.exports);\n"
    "  const eventsFactory = (exports, module) => {\n"
    "    class EventEmitter {\n"
    "      constructor() { this._events = Object.create(null); }\n"
    "      addListener(name, fn) { return this; }\n"
    "    }\n"
    "    module.exports = exports = EventEmitter;\n"
    "    exports.EventEmitter = exports;\n"
    "  };\n"
    "  const wrapped = o(eventsFactory);\n"
    "  const Events = wrapped();\n"
    "  return typeof Events + ',' + typeof Events.EventEmitter;\n"
    "})()");

  // Test 4a: Bare-events shape EXACTLY as Vite minifies it.
  // The minifier renames everything to `e`:
  //   - outer factory param `exports` → `e`
  //   - class name → `e`
  //   - some method params → `e` (shadowing the class binding)
  //   - some method bodies reference `e` (the class binding) for
  //     `e.defaultMaxListeners`
  // If Hermes has issues with TDZ/shadowing in this density of `e`
  // bindings, the outer rebind might silently fail.
  try_script(env, "vite-bare-events-minimal",
    "(function () {\n"
    "  const factory = (e, t) => {\n"
    "    t.exports = e = class e {\n"
    "      constructor() { this._events = Object.create(null); }\n"
    "      getMaxListeners() { return e.defaultMaxListeners; }\n"
    "      setMaxListeners(e) { /* shadow */ }\n"
    "      removeAllListeners(e) { return this; }\n"
    "    };\n"
    "    e.EventEmitter = e;\n"
    "    e.defaultMaxListeners = 10;\n"
    "    return typeof e + ',' + typeof e.EventEmitter + ',' + e.defaultMaxListeners;\n"
    "  };\n"
    "  const module = { exports: {} };\n"
    "  return factory(module.exports, module);\n"
    "})()");

  // Test 4b: Class-expression name shadowing the outer `e` param.
  // Vite's minifier renames both the `exports` parameter AND the
  // class name to `e`, so the actual emitted code is
  //   t.exports = e = class e { ... }
  // The class-expression scoping rules say the inner `e` is bound
  // only inside the class body — the OUTER `e` (the param) should
  // still be rebound to the class. If Hermes treats the class
  // declaration as binding `e` in the enclosing scope (instead of
  // the class body), the outer `e` rebind silently fails and the
  // subsequent `e.EventEmitter = e` errors with "Cannot set
  // property of undefined".
  try_script(env, "class-name-shadow-param",
    "(function () {\n"
    "  const factory = (e, t) => {\n"
    "    t.exports = e = class e {\n"
    "      constructor() { this.kind = 'EventEmitter'; }\n"
    "    };\n"
    "    e.EventEmitter = e;\n"
    "    return typeof e + ',' + typeof e.EventEmitter;\n"
    "  };\n"
    "  const module = { exports: {} };\n"
    "  return factory(module.exports, module);\n"
    "})()");

  // Test 5: class extends Error with a default-parameter referencing
  // the class name. This is the suspect pattern from Zapp's worker.mjs
  // (offset 559):
  //   class e extends Error { constructor(t,n,r=e,i){ super(...) } }
  // Hermes' ES6Class lowering pass crashes on this exact shape — an
  // EXC_BAD_ACCESS in ES6ClassesTransformations::makeHermesES6InternalCall.
  // Likely the self-referencing default value in the constructor
  // params interacts badly with how Hermes hoists the class binding
  // for the transformation.
  try_script(env, "class-extends-default-self-ref",
    "(function () {\n"
    "  class E extends Error {\n"
    "    constructor(t, n, r = E, i) { super(t); this.code = n; this.cls = r; }\n"
    "  }\n"
    "  const e = new E('msg', 'CODE');\n"
    "  return e.code + ',' + (e.cls === E);\n"
    "})()");

  // Test 6: Same as 5 but as a class EXPRESSION assigned to a variable.
  try_script(env, "class-expr-extends-default-self-ref",
    "(function () {\n"
    "  const C = class E extends Error {\n"
    "    constructor(t, n, r = E, i) { super(t); this.code = n; this.cls = r; }\n"
    "  };\n"
    "  const e = new C('msg', 'CODE');\n"
    "  return e.code + ',' + (e.cls === C);\n"
    "})()");

  // Test 7: Same shape but the class is the value of an arrow factory
  // call — the EXACT shape in worker.mjs's minified output.
  try_script(env, "class-expr-in-cjs-factory",
    "(function () {\n"
    "  const o = (e, t) => () => (t || e((t = {exports: {}}).exports, t), t.exports);\n"
    "  const factory = (e, t) => {\n"
    "    t.exports = class e extends Error {\n"
    "      constructor(n, r, i = e) { super(n); this.code = r; }\n"
    "    };\n"
    "  };\n"
    "  const wrapped = o(factory);\n"
    "  const C = wrapped();\n"
    "  return typeof C;\n"
    "})()");

  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));
  return 0;
}
