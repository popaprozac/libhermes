// Exercises the data-shape surface: numbers, arrays, objects.
//
// Pseudo-JS the C side simulates:
//
//   function makeUser(id, name) {
//     return { id, name, scores: [10, 20, 30] };
//   }
//   const u = makeUser(42, "ada");
//   assert(u.id === 42 && u.name === "ada" && u.scores.length === 3
//          && u.scores[1] === 20);

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[data-roundtrip] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static js_value_t *
make_user(js_env_t *env, js_callback_info_t *info) {
  size_t argc = 2;
  js_value_t *argv[2];
  CHECK(js_get_callback_info(env, info, &argc, argv, NULL, NULL));

  // Build { id, name, scores: [10, 20, 30] }
  js_value_t *user;
  CHECK(js_create_object(env, &user));
  CHECK(js_set_named_property(env, user, "id",   argv[0]));
  CHECK(js_set_named_property(env, user, "name", argv[1]));

  js_value_t *scores;
  CHECK(js_create_array_with_length(env, 3, &scores));
  for (uint32_t i = 0; i < 3; i++) {
    js_value_t *n;
    CHECK(js_create_int32(env, (int32_t) ((i + 1) * 10), &n));
    CHECK(js_set_element(env, scores, i, n));
  }
  CHECK(js_set_named_property(env, user, "scores", scores));
  return user;
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

  // Install makeUser as a global.
  js_value_t *global, *make_fn;
  CHECK(js_get_global(env, &global));
  CHECK(js_create_function(env, "makeUser", (size_t) -1, make_user, NULL, &make_fn));
  CHECK(js_set_named_property(env, global, "makeUser", make_fn));

  // Run script that exercises the resulting object.
  const char *code =
    "(function () {"
    "  const u = makeUser(42, 'ada');"
    "  if (u.id !== 42) throw new Error('bad id');"
    "  if (u.name !== 'ada') throw new Error('bad name');"
    "  if (u.scores.length !== 3) throw new Error('bad len');"
    "  if (u.scores[0] !== 10 || u.scores[1] !== 20 || u.scores[2] !== 30) throw new Error('bad scores');"
    "  return u.id + u.scores[1];" // 42 + 20 = 62
    "})()";

  js_value_t *source;
  CHECK(js_create_string_utf8(env, (const utf8_t *) code, strlen(code), &source));
  js_value_t *result;
  CHECK(js_run_script(env, "test.js", strlen("test.js"), 0, source, &result));

  // Read result back as int32. Also exercise typeof + is_number.
  js_value_type_t kind;
  CHECK(js_typeof(env, result, &kind));
  bool is_num = false;
  CHECK(js_is_number(env, result, &is_num));
  printf("[data-roundtrip] typeof result = %d (js_number=%d), is_number=%d\n",
    kind, js_number, is_num);

  int32_t n = 0;
  CHECK(js_get_value_int32(env, result, &n));
  printf("[data-roundtrip] result = %d\n", n);
  if (n != 62) {
    fprintf(stderr, "[data-roundtrip] FAIL: expected 62, got %d\n", n);
    return 1;
  }

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[data-roundtrip] OK\n");
  return 0;
}
