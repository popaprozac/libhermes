// Exercise js_define_properties + js_get_property_names — the
// exact pattern bare-* native modules use to set up their exports.
//
// Pseudo-JS the C side simulates:
//
//   const exports = {};
//   Object.defineProperties(exports, {
//     version: { value: '1.2.3' },     // value property
//     square:  { value: function(x) { return x * x; } },  // method
//     get count() { return 7; },       // accessor
//   });
//   assert(exports.version === '1.2.3');
//   assert(exports.square(4) === 16);
//   assert(exports.count === 7);
//   assert(Object.keys(exports).length >= 2);

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[define-properties] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static js_value_t *
square(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];
  CHECK(js_get_callback_info(env, info, &argc, argv, NULL, NULL));
  int32_t x = 0;
  CHECK(js_get_value_int32(env, argv[0], &x));
  js_value_t *out;
  CHECK(js_create_int32(env, x * x, &out));
  return out;
}

static js_value_t *
get_count(js_env_t *env, js_callback_info_t *info) {
  (void) info;
  js_value_t *out;
  CHECK(js_create_int32(env, 7, &out));
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

  // Build the exports object.
  js_value_t *exports;
  CHECK(js_create_object(env, &exports));

  // Three descriptors: value, method, accessor.
  js_value_t *version_str, *version_name, *square_name, *count_name;
  CHECK(js_create_string_utf8(env, (const utf8_t *) "1.2.3", 5, &version_str));
  CHECK(js_create_string_utf8(env, (const utf8_t *) "version", 7, &version_name));
  CHECK(js_create_string_utf8(env, (const utf8_t *) "square", 6, &square_name));
  CHECK(js_create_string_utf8(env, (const utf8_t *) "count", 5, &count_name));

  js_property_descriptor_t props[3] = {
    { .version = 0, .name = version_name, .data = NULL, .attributes = 0,
      .method = NULL, .getter = NULL, .setter = NULL, .value = version_str },
    { .version = 0, .name = square_name, .data = NULL, .attributes = 0,
      .method = square, .getter = NULL, .setter = NULL, .value = NULL },
    { .version = 0, .name = count_name, .data = NULL, .attributes = 0,
      .method = NULL, .getter = get_count, .setter = NULL, .value = NULL },
  };
  CHECK(js_define_properties(env, exports, props, 3));

  // Install as `mod` on global, run JS that exercises each.
  js_value_t *global;
  CHECK(js_get_global(env, &global));
  CHECK(js_set_named_property(env, global, "mod", exports));

  const char *src =
    "(function () {"
    "  if (mod.version !== '1.2.3') throw new Error('bad version: ' + mod.version);"
    "  if (mod.square(4) !== 16)    throw new Error('bad square: ' + mod.square(4));"
    "  if (mod.count !== 7)         throw new Error('bad count: ' + mod.count);"
    "  return Object.keys(mod).length;" // expect at least 2 (value + method; accessor's "configurable" not enumerable by default)
    "})()";
  js_value_t *src_v, *result;
  CHECK(js_create_string_utf8(env, (const utf8_t *) src, strlen(src), &src_v));
  CHECK(js_run_script(env, "t.js", strlen("t.js"), 0, src_v, &result));

  int32_t key_count = 0;
  CHECK(js_get_value_int32(env, result, &key_count));
  printf("[define-properties] Object.keys(mod).length = %d\n", key_count);

  // Also exercise js_get_property_names directly.
  js_value_t *names;
  CHECK(js_get_property_names(env, exports, &names));
  uint32_t names_len;
  CHECK(js_get_array_length(env, names, &names_len));
  printf("[define-properties] get_property_names returned %u entries\n", names_len);

  if (key_count < 2 || names_len < 2) {
    fprintf(stderr, "[define-properties] FAIL: expected ≥2 keys, got %d/%u\n", key_count, names_len);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[define-properties] OK\n");
  return 0;
}
