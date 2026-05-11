// Bypass-bare-and-run-the-real-bundle test.
//
// The worker.mjs Vite emits for zapp's hello-world fails inside
// the bare runtime with "Cannot set property 'EventEmitter' of
// undefined". All synthesized variants of that pattern run fine
// under libhermes in cjs-loader-pattern.c, so the question is:
// does the same large bundle also fail when fed straight to
// libhermes without bare in the picture?
//
// Compile path: read the file at runtime (path passed via argv[1]
// or a hard-coded default that points to the hello-world build),
// hand it to js_run_script, log whether eval succeeds.
//
// If this test reproduces the TypeError → the bug is inside Hermes
// (lazy compile, large-script TDZ, or similar) and we have a
// clean upstream-issue-quality repro. If it doesn't reproduce →
// the bug is in bare's plumbing somewhere.

#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CHECK(expr) do { \
  int _rc = (expr); \
  if (_rc != 0) { \
    fprintf(stderr, "[run-zapp-worker] FAIL: %s -> %d\n", #expr, _rc); \
    exit(1); \
  } \
} while (0)

static const char *default_path =
  "/Users/zach/code/zapp/hello-world/.zapp/workers/worker.mjs";

int
main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : default_path;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "[run-zapp-worker] skip: cannot open %s\n", path);
    return 0;
  }
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *buf = (char *) malloc(size + 1);
  if (fread(buf, 1, size, fp) != (size_t) size) {
    fprintf(stderr, "[run-zapp-worker] read failed\n");
    return 1;
  }
  buf[size] = '\0';
  fclose(fp);
  fprintf(stderr, "[run-zapp-worker] loaded %ld bytes from %s\n", size, path);

  uv_loop_t *loop = uv_default_loop();
  js_platform_t *platform;
  CHECK(js_create_platform(loop, NULL, &platform));
  js_env_t *env;
  CHECK(js_create_env(loop, platform, NULL, &env));
  js_handle_scope_t *scope;
  CHECK(js_open_handle_scope(env, &scope));

  js_value_t *source;
  CHECK(js_create_string_utf8(env, (const utf8_t *) buf, (size_t) size, &source));

  js_value_t *result;
  int rc = js_run_script(env, "worker.mjs", strlen("worker.mjs"), 0, source, &result);
  if (rc != 0) {
    fprintf(stderr, "[run-zapp-worker] FAILED: js_run_script returned %d "
            "(this is the repro we want for an upstream Hermes issue)\n", rc);
    free(buf);
    return 2;
  }
  fprintf(stderr, "[run-zapp-worker] PASS: bundle ran cleanly under libhermes "
          "(bug is in bare's plumbing, not Hermes)\n");

  CHECK(js_close_handle_scope(env, scope));
  CHECK(js_destroy_env(env));
  CHECK(js_destroy_platform(platform));
  free(buf);
  return 0;
}
