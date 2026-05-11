// Tighter than uv-timer-fires: mimics what bare-timers does in
// detail. Gets the loop via js_get_env_loop (not js_create_env's
// arg), registers a uv_timer on it, fires a callback that calls
// back into JS via js_call_function, then spins uv_run.
//
// If THIS passes but in-Zapp setInterval doesn't fire, the bug is
// further up (in bare's wrapper or our env→loop wiring during
// bare_setup).
//
// If this FAILS, the bug is in our js.cc — either js_get_env_loop
// returns a stale loop, uv_run interacts badly with Hermes, or
// js_call_function from a uv callback context throws.

#include <assert.h>
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct {
  js_env_t *env;
  js_ref_t *callback_ref;
  int fires;
} ctx_t;

static void
on_timer(uv_timer_t *handle) {
  ctx_t *ctx = (ctx_t *) handle->data;
  ctx->fires++;
  fprintf(stderr, "[uv-timer-via-env] uv callback #%d entered\n", ctx->fires);

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(ctx->env, &scope);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(ctx->env, ctx->callback_ref, &callback);
  assert(err == 0);

  js_value_t *global;
  err = js_get_global(ctx->env, &global);
  assert(err == 0);

  fprintf(stderr, "[uv-timer-via-env] calling JS callback...\n");
  err = js_call_function(ctx->env, global, callback, 0, NULL, NULL);
  fprintf(stderr, "[uv-timer-via-env] js_call_function returned %d\n", err);

  err = js_close_handle_scope(ctx->env, scope);
  assert(err == 0);

  if (ctx->fires >= 3) {
    uv_timer_stop(handle);
    uv_close((uv_handle_t *) handle, NULL);
  }
}

static js_value_t *
js_callback_fn(js_env_t *env, js_callback_info_t *info) {
  (void) info;
  fprintf(stderr, "[uv-timer-via-env] JS callback FIRED from C\n");
  return NULL;
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

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  // Get loop the way bare-timers does — via js_get_env_loop, NOT
  // the loop we passed to js_create_env. They should be the same;
  // this proves it.
  uv_loop_t *env_loop;
  err = js_get_env_loop(env, &env_loop);
  assert(err == 0);
  if (env_loop != loop) {
    fprintf(stderr, "[uv-timer-via-env] FAIL: env loop (%p) != input loop (%p)\n",
            (void *) env_loop, (void *) loop);
    return 1;
  }
  fprintf(stderr, "[uv-timer-via-env] env loop == input loop OK\n");

  // Create a real JS function (mimics bare-timers' on_timeout ref).
  js_value_t *callback;
  err = js_create_function(env, "tick", -1, js_callback_fn, NULL, &callback);
  assert(err == 0);

  js_ref_t *callback_ref;
  err = js_create_reference(env, callback, 1, &callback_ref);
  assert(err == 0);

  ctx_t ctx = { .env = env, .callback_ref = callback_ref, .fires = 0 };

  uv_timer_t timer;
  err = uv_timer_init(env_loop, &timer);
  assert(err == 0);
  timer.data = &ctx;

  err = uv_timer_start(&timer, on_timer, 100, 100);
  assert(err == 0);
  fprintf(stderr, "[uv-timer-via-env] timer registered, entering uv_run...\n");

  err = uv_run(env_loop, UV_RUN_DEFAULT);
  fprintf(stderr, "[uv-timer-via-env] uv_run returned %d, fires=%d\n",
          err, ctx.fires);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  if (ctx.fires < 3) {
    fprintf(stderr, "[uv-timer-via-env] FAIL: expected >= 3 fires, got %d\n",
            ctx.fires);
    return 1;
  }
  fprintf(stderr, "[uv-timer-via-env] PASS\n");
  return 0;
}
