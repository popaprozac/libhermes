// Two-way exception flow:
//
// 1. Host throws → JS catches. A C callback calls
//    js_throw_type_error; the host_function dispatch re-throws as
//    jsi::JSError; JS try/catch picks it up.
//
// 2. JS throws → host receives. A script does `throw new Error(...)`;
//    js_run_script's catch routes it to the env->on_uncaught
//    callback we register.
//
// Both paths matter for Bare: native modules need to throw cleanly
// to JS callers, and the worker bootstrap needs to receive
// uncaught script errors to dispatch worker:crashed.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[exception-flow] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

// Host function that always throws a TypeError.
static js_value_t *
do_throw(js_env_t *env, js_callback_info_t *info) {
  (void) info;
  js_throw_type_error(env, "BAD_ARG", "deliberately thrown from host");
  return NULL;
}

// Counter incremented when env->on_uncaught fires. Test asserts it.
static int uncaught_count = 0;
static char uncaught_message[256];

static void
on_uncaught(js_env_t *env, js_value_t *error, void *data) {
  (void) data;
  uncaught_count++;
  // Pull `.message` off the Error object.
  js_value_t *msg_v;
  if (js_get_named_property(env, error, "message", &msg_v) == 0) {
    size_t len = 0;
    js_get_value_string_utf8(env, msg_v, (utf8_t *) uncaught_message, sizeof(uncaught_message) - 1, &len);
    uncaught_message[len] = '\0';
  }
}

int
main(void) {
  uv_loop_t *loop = uv_default_loop();
  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));
  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));
  CHECK(js_on_uncaught_exception(env, on_uncaught, NULL));
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  js_value_t *global;
  CHECK(js_get_global(env, &global));

  // ─── Direction 1: host throw → JS catches ───
  js_value_t *throw_fn;
  CHECK(js_create_function(env, "doThrow", (size_t) -1, do_throw, NULL, &throw_fn));
  CHECK(js_set_named_property(env, global, "doThrow", throw_fn));

  const char *catchSrc =
    "(function () {"
    "  try { doThrow(); return 'should-not-reach'; }"
    "  catch (e) { return e.constructor.name + ': ' + e.message + ' (code=' + e.code + ')'; }"
    "})()";
  js_value_t *src1;
  CHECK(js_create_string_utf8(env, (const utf8_t *) catchSrc, strlen(catchSrc), &src1));
  js_value_t *result1;
  CHECK(js_run_script(env, "test.js", strlen("test.js"), 0, src1, &result1));

  char buf[256]; size_t len = 0;
  CHECK(js_get_value_string_utf8(env, result1, (utf8_t *) buf, sizeof(buf) - 1, &len));
  buf[len] = '\0';
  printf("[exception-flow] host→JS: '%s'\n", buf);
  if (strstr(buf, "TypeError") == NULL ||
      strstr(buf, "deliberately") == NULL ||
      strstr(buf, "BAD_ARG") == NULL) {
    fprintf(stderr, "[exception-flow] FAIL: expected TypeError + message + code, got '%s'\n", buf);
    return 1;
  }

  // ─── Direction 2: JS throws → host on_uncaught receives ───
  // No surrounding try/catch in the script; error escapes
  // evaluateJavaScript, which our js_run_script catches and
  // dispatches to env->on_uncaught.
  const char *throwSrc = "throw new Error('escape!');";
  js_value_t *src2;
  CHECK(js_create_string_utf8(env, (const utf8_t *) throwSrc, strlen(throwSrc), &src2));
  int rc = js_run_script(env, "test2.js", strlen("test2.js"), 0, src2, NULL);
  printf("[exception-flow] JS→host: rc=%d, on_uncaught fired %d times, msg='%s'\n",
    rc, uncaught_count, uncaught_message);
  if (rc != -1) {
    fprintf(stderr, "[exception-flow] FAIL: expected run_script to return -1, got %d\n", rc);
    return 1;
  }
  if (uncaught_count != 1 || strcmp(uncaught_message, "escape!") != 0) {
    fprintf(stderr, "[exception-flow] FAIL: on_uncaught: count=%d msg='%s'\n",
      uncaught_count, uncaught_message);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[exception-flow] OK\n");
  return 0;
}
