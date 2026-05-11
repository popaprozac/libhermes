// End-to-end: C installs a host function on globalThis, then runs
// a JS script that calls it and returns the result. This is the
// exact pattern Bare uses for native modules — `binding.c` builds
// a function object and bare-module attaches it to the addon's
// exports.
//
// Pseudo-JS:
//   globalThis.shout = /* C function */;
//   (function() { return shout("hi") + "!"; })()
//   // → "HI!"

#include <ctype.h>
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[global-host-binding] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static js_value_t *
shout(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  CHECK(js_get_callback_info(env, info, &argc, argv, NULL, NULL));

  if (argc < 1) {
    js_value_t *empty;
    js_create_string_utf8(env, (const utf8_t *) "", 0, &empty);
    return empty;
  }

  char buf[256];
  size_t len = 0;
  js_get_value_string_utf8(env, argv[0], (utf8_t *) buf, sizeof(buf) - 1, &len);
  buf[len] = '\0';

  // Uppercase in place. Simple loop — proves we're doing real C
  // work on data that came from JS and is going back to JS.
  for (size_t i = 0; i < len; i++) buf[i] = (char) toupper((unsigned char) buf[i]);

  js_value_t *out;
  js_create_string_utf8(env, (const utf8_t *) buf, len, &out);
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

  // Attach `shout` to globalThis so JS code can call it directly.
  js_value_t *global;
  CHECK(js_get_global(env, &global));

  js_value_t *shout_fn;
  CHECK(js_create_function(env, "shout", (size_t) -1, shout, NULL, &shout_fn));
  CHECK(js_set_named_property(env, global, "shout", shout_fn));

  // Now run JS that exercises it.
  const char *code = "(function () { return shout('hi') + '!'; })()";
  js_value_t *source;
  CHECK(js_create_string_utf8(env, (const utf8_t *) code, strlen(code), &source));

  js_value_t *result;
  CHECK(js_run_script(env, "test.js", strlen("test.js"), 0, source, &result));

  size_t len = 0;
  char buf[64];
  CHECK(js_get_value_string_utf8(env, result, (utf8_t *) buf, sizeof(buf) - 1, &len));
  buf[len] = '\0';

  printf("[global-host-binding] result: '%s'\n", buf);
  if (strcmp(buf, "HI!") != 0) {
    fprintf(stderr, "[global-host-binding] FAIL: expected 'HI!', got '%s'\n", buf);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[global-host-binding] OK\n");
  return 0;
}
