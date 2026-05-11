// Smoke test: a vanilla uv_timer_t scheduled against the same uv_loop
// we hand to libhermes' js_create_env. Confirms that timers DO fire
// when our libhermes is on the platform — narrows whether the
// "setInterval not firing" issue in Zapp is a uv-vs-Hermes
// interaction problem or somewhere higher up the stack (bare's
// timer binding, JS-side scheduler bookkeeping, etc.).

#include <assert.h>
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static int g_fires = 0;

static void
on_timer(uv_timer_t *handle) {
  g_fires++;
  fprintf(stderr, "[uv-timer-fires] timer fired #%d\n", g_fires);
  if (g_fires >= 3) {
    uv_timer_stop(handle);
    uv_close((uv_handle_t *) handle, NULL);
  }
}

int
main(void) {
  uv_loop_t *loop = uv_default_loop();

  js_platform_t *platform;
  int err = js_create_platform(loop, NULL, &platform);
  assert(err == 0);

  js_env_t *env;
  err = js_create_env(loop, platform, NULL, &env);
  assert(err == 0);

  uv_timer_t timer;
  err = uv_timer_init(loop, &timer);
  assert(err == 0);
  // 100ms initial delay, 100ms repeat.
  err = uv_timer_start(&timer, on_timer, 100, 100);
  assert(err == 0);

  fprintf(stderr, "[uv-timer-fires] entering uv_run...\n");
  err = uv_run(loop, UV_RUN_DEFAULT);
  fprintf(stderr, "[uv-timer-fires] uv_run returned %d, fires=%d\n", err, g_fires);

  if (g_fires < 3) {
    fprintf(stderr, "[uv-timer-fires] FAIL: expected >= 3 timer fires, got %d\n", g_fires);
    return 1;
  }
  fprintf(stderr, "[uv-timer-fires] PASS\n");
  return 0;
}
