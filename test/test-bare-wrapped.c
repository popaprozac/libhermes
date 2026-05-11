// Run the bare bundle the way bare.c does: wrap as (function(bare, require){<bundle>})
// and call with (exports_object, require_stub).

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[test] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static js_value_t *
require_stub(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  char name[256];
  size_t len = 0;
  if (argc > 0) {
    js_get_value_string_utf8(env, argv[0], (utf8_t *)name, sizeof(name), &len);
    name[len < sizeof(name) ? len : sizeof(name)-1] = '\0';
  }
  fprintf(stderr, "[test] require(%s) — returning empty object\n", name);
  js_value_t *empty;
  js_create_object(env, &empty);
  return empty;
}

int
main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/bare-bundle.js";
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "[test] cannot open %s\n", path); return 1; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(size + 1);
  fread(buf, 1, size, f);
  buf[size] = 0;
  fclose(f);

  uv_loop_t *loop = uv_default_loop();
  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));
  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  // Build args: "bare", "require"
  js_value_t *source;
  CHECK(js_create_string_utf8(env, (utf8_t *)buf, size, &source));
  js_value_t *args[2];
  CHECK(js_create_string_utf8(env, (utf8_t *)"bare", -1, &args[0]));
  CHECK(js_create_string_utf8(env, (utf8_t *)"require", -1, &args[1]));

  js_value_t *entry;
  int rc = js_create_function_with_source(env, NULL, 0, "bare:/bare.js", -1, args, 2, 0, source, &entry);
  if (rc != 0) {
    fprintf(stderr, "[test] create_function_with_source failed: %d\n", rc);
    return 2;
  }
  fprintf(stderr, "[test] bare.js compiled OK\n");

  // Create exports object + require stub. Bare expects
  // `require.addon` to exist as a function on the require fn it
  // gets — replicate that here so bare-os/bare-buffer/etc. binding
  // modules don't trip on `__bundle.builtinRequire.addon`.
  js_value_t *exports;
  CHECK(js_create_object(env, &exports));
  js_value_t *require_fn;
  CHECK(js_create_function(env, "require", -1, require_stub, NULL, &require_fn));
  js_value_t *addon_fn;
  CHECK(js_create_function(env, "addon", -1, require_stub, NULL, &addon_fn));
  CHECK(js_set_named_property(env, require_fn, "addon", addon_fn));

  // Call entry(exports, require)
  js_value_t *call_args[2] = { exports, require_fn };
  js_value_t *global;
  CHECK(js_get_global(env, &global));

  // Bare's runtime.c does this — without it, bare/src/bare.js
  // throws a ReferenceError on its first `global.X` access.
  CHECK(js_set_named_property(env, global, "global", global));

  fprintf(stderr, "[test] calling entry(exports, require)...\n");
  rc = js_call_function(env, global, entry, 2, call_args, NULL);
  if (rc != 0) {
    fprintf(stderr, "[test] entry call returned %d\n", rc);
  } else {
    fprintf(stderr, "[test] entry call completed OK\n");
  }

  return 0;
}
