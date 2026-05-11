// Smoke test for js_create_function + js_call_function +
// js_get_callback_info: round-trip an argument through a C host
// function. Validates the JSI HostFunction binding.
//
// Pseudo-JS the C side simulates:
//
//   function greet(name) {
//     // C body: argv[0] is the string; we ignore it for now and
//     // return "hello, from host" as a hardcoded constant. Just
//     // need to confirm the call path works.
//     return "hello, from host";
//   }
//   const result = greet("world");
//   // result === "hello, from host"

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[host-function] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static js_value_t *
greet(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  CHECK(js_get_callback_info(env, info, &argc, argv, NULL, NULL));

  // Receive a printable arg to prove the bridge passes them, even
  // though we ignore the value for the assertion.
  if (argc >= 1) {
    char buf[64];
    size_t len = 0;
    js_get_value_string_utf8(env, argv[0], (utf8_t *) buf, sizeof(buf) - 1, &len);
    buf[len] = '\0';
    fprintf(stderr, "[host-function] greet() called with arg[0]='%s'\n", buf);
  }

  const char *msg = "hello, from host";
  js_value_t *out;
  CHECK(js_create_string_utf8(env, (const utf8_t *) msg, strlen(msg), &out));
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

  // Create the host function.
  js_value_t *greet_fn;
  CHECK(js_create_function(env, "greet", (size_t) -1, greet, NULL, &greet_fn));

  // Build an arg ("world") and call it.
  const char *arg_text = "world";
  js_value_t *arg0;
  CHECK(js_create_string_utf8(env, (const utf8_t *) arg_text, strlen(arg_text), &arg0));

  js_value_t *args[] = {arg0};
  js_value_t *result;
  CHECK(js_call_function(env, NULL, greet_fn, 1, args, &result));

  size_t len = 0;
  char buf[64];
  CHECK(js_get_value_string_utf8(env, result, (utf8_t *) buf, sizeof(buf) - 1, &len));
  buf[len] = '\0';

  printf("[host-function] result: '%s'\n", buf);
  if (strcmp(buf, "hello, from host") != 0) {
    fprintf(stderr, "[host-function] FAIL: expected 'hello, from host', got '%s'\n", buf);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[host-function] OK\n");
  return 0;
}
