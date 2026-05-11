// ArrayBuffer + external + references in one place.
//
// 1. Allocate an ArrayBuffer of N bytes from C, fill with a pattern.
// 2. Store a persistent reference to it (outlives the handle scope).
// 3. Run JS that reads from it: build a Uint8Array view, sum bytes.
// 4. Close the original scope, open a new one, recover the
//    ArrayBuffer from the reference, mutate it from C, run JS
//    again and verify the new contents.
//
// Exercises the surface every bare-buffer / bare-fetch binding leans
// on: ArrayBuffer C↔JS handoff with the C side keeping a long-lived
// handle.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[ab-roundtrip] FAIL: %s -> %d\n", #expr, _rc); \
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

  js_handle_scope_t *scope1;
  CHECK(js_open_handle_scope(env, &scope1));

  // Step 1: allocate, fill with pattern.
  void *raw;
  js_value_t *ab;
  CHECK(js_create_arraybuffer(env, 8, &raw, &ab));

  bool is_ab = false;
  CHECK(js_is_arraybuffer(env, ab, &is_ab));
  if (!is_ab) { fprintf(stderr, "[ab-roundtrip] FAIL: is_arraybuffer false\n"); return 1; }

  uint8_t *bytes = (uint8_t *) raw;
  for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(i + 1); // 1..8

  // Verify via js_get_arraybuffer_info that we read the same bytes.
  void *seen_data; size_t seen_len;
  CHECK(js_get_arraybuffer_info(env, ab, &seen_data, &seen_len));
  if (seen_len != 8 || seen_data != raw) {
    fprintf(stderr, "[ab-roundtrip] FAIL: info mismatch len=%zu data_eq=%d\n",
      seen_len, seen_data == raw);
    return 1;
  }

  // Step 2: take a persistent reference so the buffer survives
  // crossing scope boundaries on the C side.
  js_ref_t *ab_ref;
  CHECK(js_create_reference(env, ab, 1, &ab_ref));

  // Step 3: expose as `buf` on global, run JS that sums bytes.
  js_value_t *global;
  CHECK(js_get_global(env, &global));
  CHECK(js_set_named_property(env, global, "buf", ab));

  const char *sumSrc =
    "(function () {"
    "  const u = new Uint8Array(buf);"
    "  let s = 0;"
    "  for (let i = 0; i < u.length; i++) s += u[i];"
    "  return s;"
    "})()";
  js_value_t *src;
  CHECK(js_create_string_utf8(env, (const utf8_t *) sumSrc, strlen(sumSrc), &src));
  js_value_t *r1;
  CHECK(js_run_script(env, "sum.js", strlen("sum.js"), 0, src, &r1));

  int32_t sum1 = 0;
  CHECK(js_get_value_int32(env, r1, &sum1));
  printf("[ab-roundtrip] sum of [1..8] = %d (expected 36)\n", sum1);
  if (sum1 != 36) { fprintf(stderr, "[ab-roundtrip] FAIL: bad sum1\n"); return 1; }

  // Step 4: close scope1, open scope2, recover via reference.
  CHECK(js_close_handle_scope(env, scope1));
  js_handle_scope_t *scope2;
  CHECK(js_open_handle_scope(env, &scope2));

  js_value_t *ab2;
  CHECK(js_get_reference_value(env, ab_ref, &ab2));

  // Mutate in place to all 0xFF.
  void *raw2; size_t len2;
  CHECK(js_get_arraybuffer_info(env, ab2, &raw2, &len2));
  memset(raw2, 0xFF, len2);

  // Re-install on global and re-run the same script — should get
  // 8 * 255 = 2040.
  js_value_t *global2;
  CHECK(js_get_global(env, &global2));
  CHECK(js_set_named_property(env, global2, "buf", ab2));

  js_value_t *src2;
  CHECK(js_create_string_utf8(env, (const utf8_t *) sumSrc, strlen(sumSrc), &src2));
  js_value_t *r2;
  CHECK(js_run_script(env, "sum.js", strlen("sum.js"), 0, src2, &r2));

  int32_t sum2 = 0;
  CHECK(js_get_value_int32(env, r2, &sum2));
  printf("[ab-roundtrip] sum after mutate = %d (expected 2040)\n", sum2);
  if (sum2 != 2040) { fprintf(stderr, "[ab-roundtrip] FAIL: bad sum2\n"); return 1; }

  CHECK(js_delete_reference(env, ab_ref));
  CHECK(js_close_handle_scope(env, scope2));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));

  printf("[ab-roundtrip] OK\n");
  return 0;
}
