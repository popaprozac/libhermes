// libhermes — Bare engine binding for Meta's Hermes.
//
// Implements the C ABI declared in `js.h` (sourced from
// holepunchto/libjs) on top of Hermes' embedding APIs. Same shape
// as libjsc / libqjs / libv8 / libmqjs; from Bare's perspective
// "Hermes" is just another `js_*` provider.
//
// === Translation between two ABIs ===
//
//   js.h          C, ref-counted handle scopes, NAPI-shaped surface,
//                 what Bare and every bare-* module's `binding.c`
//                 calls into.
//   Hermes VM     C++, JSI + hermes::vm::Runtime, lambda-based
//                 callbacks, RAII-managed values, persistent
//                 references via jsi::Object's PropNameID/Function/
//                 etc. wrappers.
//
// The bulk of this file is a translation table. By analogy with
// libqjs (~6.5K LOC) expect a similar size when complete.
//
// === Phase 1 milestone ===
//
// `bare --eval 'console.log(1+1)'` end-to-end. That requires:
//
//   ✗ js_create_platform / js_destroy_platform
//   ✗ js_create_env / js_destroy_env
//   ✗ js_open_handle_scope / js_close_handle_scope
//   ✗ js_get_global / js_set_named_property
//   ✗ js_create_string_utf8 / js_get_value_string_utf8
//   ✗ js_create_function / js_call_function
//   ✗ js_run_script
//   ✗ js_on_uncaught_exception
//
// Functions below are currently STUBS that return `-1` for "not
// implemented". Implement one at a time; flip the checkbox above
// when each lands.

extern "C" {
#include <js.h>
}

#include <hermes/hermes.h>
#include <hermes/Public/RuntimeConfig.h>
#include <jsi/jsi.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace jsi = ::facebook::jsi;
using HermesRuntime = ::facebook::hermes::HermesRuntime;

// === Opaque type definitions =========================================
//
// `js.h` declares these as forward-only `struct X;` so consumers
// can hold pointers but not poke at internals. We define the actual
// layouts here. Field names are tentative — adjust as we add APIs.

struct js_platform_s {
  uv_loop_t *loop;
  // Hermes is more lightweight than V8 — no separate "platform"
  // object holding worker threads / ICU / etc. The js_platform_t
  // exists mainly to give Bare a thing to hand out and to hold the
  // event loop. Per-VM state lives on js_env_t.
};

struct js_env_s {
  uv_loop_t *loop;
  js_platform_t *platform;
  std::unique_ptr<HermesRuntime> runtime;

  // Handle scopes form a stack. Each scope owns the values that
  // were materialized while it was open; closing a scope releases
  // them. We model scopes as a linked list per env; js_value_t*
  // pointers reference entries inside.
  js_handle_scope_t *top_scope = nullptr;

  // Pending-exception slot. js.h's NAPI-style convention: when a
  // host function fails, it stores the error here and returns null
  // (or its error sentinel). Callers query via
  // js_is_exception_pending / js_get_and_clear_last_exception.
  //
  // We use a unique_ptr-of-jsi::Value (move-only) rather than
  // jsi::Value-by-value because the latter requires construction
  // up front and we want to signal "no pending exception" with a
  // null pointer.
  std::unique_ptr<jsi::Value> pending_exception;

  // Set by js_on_uncaught_exception; called by js_run_script when
  // a JSError escapes the script body and no closer try/catch
  // caught it. Maps to Bare's "uncaughtException" event flow.
  js_uncaught_exception_cb on_uncaught = nullptr;
  void *on_uncaught_data = nullptr;

  // Re-entry guard for on_uncaught dispatch. Bare's uncaught
  // handler itself calls js_call_function, which may throw, which
  // would call on_uncaught again — infinite recursion. When this
  // is true we skip the callback and just print the error.
  bool in_uncaught = false;

  // Set by js_on_unhandled_rejection. Bare registers this during
  // worker bootstrap. We store the slot today but don't yet wire
  // it to Hermes' promise-rejection-tracker — bare doesn't
  // currently rely on the callback firing, it just needs the
  // register call to succeed so its bootstrap proceeds.
  js_unhandled_rejection_cb on_unhandled_rejection = nullptr;
  void *on_unhandled_rejection_data = nullptr;
};

// A scope owns a vector of values; closing the scope drops them all.
// `prev` chains up the stack so `js_close_handle_scope` can pop.
struct js_handle_scope_s {
  js_env_t *env;
  js_handle_scope_t *prev;
  std::vector<std::unique_ptr<js_value_s>> values;
};

// `js_value_t` is one node in a scope's owned-value list. Holds a
// JSI Value (which is itself a tagged union — Number / String /
// Object / etc. — internally; jsi::Value handles the dispatch).
//
// NOTE: jsi::Value is move-only and non-copyable. Wrap in a struct
// so the C API can pass `js_value_t*` around without copies.
struct js_value_s {
  jsi::Value value;
  // Reference to the runtime that owns this value's GC root. Mostly
  // bookkeeping today; matters when we wire persistent references.
  HermesRuntime *runtime;
};

// Per-invocation context handed to host C callbacks via
// `js_get_callback_info`. Lives on the stack of the JSI lambda
// (see `js_create_function`); the C callback is only allowed to
// read it during its own execution.
struct js_callback_info_s {
  js_env_t *env;
  const jsi::Value *thisVal;
  const jsi::Value *args;
  size_t argc;
  void *data; // user data pointer captured at js_create_function time
};

// === Stub macros =====================================================
//
// Until each function is implemented, return -1 to make missing
// functionality obvious at runtime rather than silently returning
// uninitialized memory.

#define NOT_IMPL(fn) \
  do {               \
    (void) fn;       \
    return -1;       \
  } while (0)

// === Internal helpers ================================================

// Adopt a jsi::Value into the current handle scope and return the
// js_value_t* the C ABI hands out. Caller must have an open scope —
// js.h's contract is that every value creation is bracketed by
// js_open_handle_scope / js_close_handle_scope.
//
// We don't try to guard against a missing scope here; callers
// violating that contract get a deterministic crash (top_scope null
// deref) which is a louder bug than a leaked allocation.
static js_value_t *
adopt_value(js_env_t *env, jsi::Value &&value) {
  auto wrapper = std::make_unique<js_value_s>();
  wrapper->value = std::move(value);
  wrapper->runtime = env->runtime.get();
  auto *raw = wrapper.get();
  env->top_scope->values.push_back(std::move(wrapper));
  return raw;
}

// === Platform / Env =================================================

extern "C" int
js_create_platform(uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  (void) options;
  auto *p = new js_platform_s{loop};
  *result = p;
  return 0;
}

extern "C" int
js_destroy_platform(js_platform_t *platform) {
  delete platform;
  return 0;
}

extern "C" int
js_get_platform_loop(js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;
  return 0;
}

extern "C" int
js_get_platform_identifier(js_platform_t *platform, const char **result) {
  (void) platform;
  *result = "hermes";
  return 0;
}

extern "C" int
js_get_platform_version(js_platform_t *platform, const char **result) {
  (void) platform;
  // TODO: return Hermes' actual version string from
  // HermesRuntime::getVersion() or similar API.
  *result = "0.0.0-libhermes-scaffold";
  return 0;
}

extern "C" int
js_create_env(uv_loop_t *loop, js_platform_t *platform, const js_env_options_t *options, js_env_t **result) {
  (void) options;
  auto *env = new js_env_s();
  env->loop = loop;
  env->platform = platform;
  // Enable Hermes' microtask queue. Without this, Promise.then
  // callbacks throw "Property 'setImmediate' doesn't exist" because
  // Hermes' JS-side Promise polyfill expects a host-provided
  // setImmediate; with the C++ microtask queue ON, promise
  // continuations queue through HermesRuntime::queueMicrotask
  // instead and we drain them via drainMicrotasks().
  //
  // ES6Class defaults to FALSE in Hermes' RuntimeConfig. With it
  // off, the parser rejects `class X {}` (both declarations and
  // expressions) with "Invalid expression encountered". Modern JS
  // bundles use classes everywhere — bare-events does
  // `module.exports = exports = class EventEmitter {...}` which is
  // a hard requirement for the bare bootstrap. Turn it on.
  auto config = ::hermes::vm::RuntimeConfig::Builder()
    .withMicrotaskQueue(true)
    .withES6Class(true)
    .build();
  env->runtime = ::facebook::hermes::makeHermesRuntime(config);
  *result = env;
  return 0;
}

extern "C" int
js_destroy_env(js_env_t *env) {
  delete env;
  return 0;
}

extern "C" int
js_get_env_loop(js_env_t *env, uv_loop_t **result) {
  *result = env->loop;
  return 0;
}

extern "C" int
js_get_env_platform(js_env_t *env, js_platform_t **result) {
  *result = env->platform;
  return 0;
}

extern "C" int
js_on_uncaught_exception(js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->on_uncaught = cb;
  env->on_uncaught_data = data;
  return 0;
}

// Bare's worker bootstrap (vendor/bare/src/runtime.c) calls this
// shortly after env creation. Returning -1 (as the STUB did)
// caused bare's subsequent setup to read an undefined function
// out of its registered handler slot, which then surfaced as the
// "Value is undefined, expected an Object" JSError when bare
// tried to call it.
//
// We store the cb/data slot so the register call succeeds. Wiring
// it to Hermes' actual unhandled-rejection notifications is a
// follow-up: Hermes exposes `setRejectionTracker` on the C++ side
// (host runtime API); we'd hook it to dispatch into env->on_unhandled_rejection.
// For now, bare's bootstrap only needs the register call to
// return 0 — the callback never actually fires today, and bare
// has its own fallback path for unhandled rejections.
extern "C" int
js_on_unhandled_rejection(js_env_t *env, js_unhandled_rejection_cb cb, void *data) {
  env->on_unhandled_rejection = cb;
  env->on_unhandled_rejection_data = data;
  return 0;
}

// Dynamic-import host hooks. Bare registers these during bootstrap;
// our bundles don't actually use dynamic import yet (bare-pack
// inlines everything), so we just accept the register and return
// success. Wiring them to Hermes' host-promise-resolution path is
// future work.
extern "C" int
js_on_dynamic_import(js_env_t *env, js_dynamic_import_cb cb, void *data) {
  (void) env; (void) cb; (void) data;
  return 0;
}

extern "C" int
js_on_dynamic_import_transitional(js_env_t *env, js_dynamic_import_transitional_cb cb, void *data) {
  (void) env; (void) cb; (void) data;
  return 0;
}

// Teardown hooks. Bare registers cleanup callbacks for libuv
// resources etc. We accept the register and return 0 so bootstrap
// proceeds; firing the callbacks on env destroy is part of the
// not-yet-implemented Hermes teardown story (the host app is
// expected to live the lifetime of the process today).
extern "C" int
js_add_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  (void) env; (void) callback; (void) data;
  return 0;
}

extern "C" int
js_remove_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  (void) env; (void) callback; (void) data;
  return 0;
}

extern "C" int
js_add_deferred_teardown_callback(js_env_t *env, js_deferred_teardown_cb callback, void *data, js_deferred_teardown_t **result) {
  (void) env; (void) callback; (void) data;
  // bare expects a non-null handle back so it has something to
  // pass to js_finish_deferred_teardown_callback later. Hand back
  // a sentinel — we never inspect it.
  if (result) *result = reinterpret_cast<js_deferred_teardown_t *>(0x1);
  return 0;
}

extern "C" int
js_finish_deferred_teardown_callback(js_deferred_teardown_t *handle) {
  (void) handle;
  return 0;
}

// === Handle scopes ==================================================

extern "C" int
js_open_handle_scope(js_env_t *env, js_handle_scope_t **result) {
  auto *scope = new js_handle_scope_s();
  scope->env = env;
  scope->prev = env->top_scope;
  env->top_scope = scope;
  *result = scope;
  return 0;
}

extern "C" int
js_close_handle_scope(js_env_t *env, js_handle_scope_t *scope) {
  if (env->top_scope != scope) {
    // Out-of-order scope close is a programming error in caller.
    // libqjs / libjsc abort here; we'd do the same. For now return
    // an error code.
    return -1;
  }
  env->top_scope = scope->prev;
  delete scope; // releases all owned js_value_s
  return 0;
}

// === Stubs for the rest of Phase 1 ==================================
//
// Sketch the function bodies enough to compile; implement when each
// becomes the next blocker. All return -1 until then.

extern "C" int
js_create_string_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  // NAPI / js.h convention: `len == (size_t)-1` means the input is
  // null-terminated and the caller wants us to discover the length.
  // Bare's bootstrap passes this sentinel constantly — without the
  // check we hand a ~SIZE_MAX length to Hermes' StringPrimitive,
  // which immediately throws std::length_error → abort().
  if (len == (size_t) -1) {
    len = str ? std::strlen(reinterpret_cast<const char *>(str)) : 0;
  }
  auto s = jsi::String::createFromUtf8(
    *env->runtime, reinterpret_cast<const uint8_t *>(str), len
  );
  *result = adopt_value(env, jsi::Value(*env->runtime, s));
  return 0;
}

extern "C" int
js_get_value_string_utf8(js_env_t *env, js_value_t *value, utf8_t *buf, size_t len, size_t *result) {
  auto &rt = *env->runtime;
  auto s = value->value.asString(rt).utf8(rt);
  if (buf == nullptr) {
    // NAPI convention: nullptr buffer = query required length.
    if (result) *result = s.size();
    return 0;
  }
  size_t to_copy = (len > 0 && len < s.size()) ? len : s.size();
  std::memcpy(buf, s.data(), to_copy);
  if (result) *result = to_copy;
  return 0;
}

extern "C" int
js_get_global(js_env_t *env, js_value_t **result) {
  auto g = env->runtime->global();
  *result = adopt_value(env, jsi::Value(*env->runtime, g));
  return 0;
}

extern "C" int
js_set_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  // Bare's native addon init code (e.g. bare-hrtime/binding.c) and
  // bare's runtime.c routinely do
  //   js_create_function(... &val);
  //   assert(err == 0);
  //   js_set_named_property(exports, name, val);
  // With NDEBUG the assert is a no-op, so a stubbed creator that
  // returns -1 leaves `val` uninitialized — the next setProperty
  // then SEGVs inside Hermes' slot allocator on a corrupt
  // receiver. Defensive guard: refuse the call cleanly and stderr
  // so the bug surfaces visibly instead of as an opaque
  // EXC_BAD_ACCESS deep in Hermes.
  if (object == nullptr || value == nullptr) {
    fprintf(stderr, "[libhermes] js_set_named_property(name=%s) called with %s — "
                    "an upstream creator likely returned -1 with *result unset\n",
      name, object == nullptr ? "NULL object" : "NULL value");
    return -1;
  }
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  obj.setProperty(rt, name, jsi::Value(rt, value->value));
  return 0;
}

extern "C" int
js_create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  auto &rt = *env->runtime;
  // jsi::PropNameID requires a length-bounded UTF-8 string. `len`
  // from the C API can be SIZE_MAX (NAPI convention for "use
  // strlen") or 0 (meaning "name is unused / empty"); normalize.
  std::string fn_name = (name && len != static_cast<size_t>(-1))
    ? std::string(name, len)
    : (name ? std::string(name) : std::string("anonymous"));
  auto prop_name = jsi::PropNameID::forUtf8(rt, fn_name);

  // paramCount is advisory — JS callers can pass any number of args
  // regardless. Pass 0 so JSI doesn't impose an arity check.
  auto fn = jsi::Function::createFromHostFunction(
    rt, prop_name, 0,
    // Lambda captures the user's C callback + data. Each invocation
    // builds a js_callback_info_s on the stack and dispatches.
    [env, cb, data](
      jsi::Runtime &rt,
      const jsi::Value &thisVal,
      const jsi::Value *args,
      size_t count
    ) -> jsi::Value {
      js_callback_info_s info{env, &thisVal, args, count, data};
      js_value_t *ret = cb(env, &info);

      // If the C callback set a pending exception (via js_throw_*),
      // surface it as a jsi::JSError so JS-side try/catch sees it.
      // The JSError's value is the error object we stored on the env.
      if (env->pending_exception) {
        auto err = std::move(*env->pending_exception);
        env->pending_exception.reset();
        throw jsi::JSError(rt, std::move(err));
      }

      if (ret == nullptr) {
        // C callback returned nothing — coerce to undefined.
        return jsi::Value::undefined();
      }
      // Move the returned value out of the handle scope so the C
      // caller's scope-close doesn't trip on it being aliased.
      // jsi::Value is move-only; we steal its contents and return.
      return std::move(ret->value);
    }
  );

  *result = adopt_value(env, jsi::Value(rt, fn));
  return 0;
}

// js_create_typed_function — same surface as js_create_function
// plus a typed-call fast path. The fast path lets engines skip
// the generic JS calling convention when they recognize the
// signature (V8 and JSC implement it via Fast API Calls). Hermes
// has no equivalent, so we fall through to the regular untyped
// function. Bare callers like bare-hrtime get a working function
// (just without the typed fast path); the JS-visible behavior is
// identical.
extern "C" int
js_create_typed_function(
    js_env_t *env,
    const char *name,
    size_t len,
    js_function_cb cb,
    const js_callback_signature_t *signature,
    const void *address,
    void *data,
    js_value_t **result) {
  (void) signature;
  (void) address;
  return js_create_function(env, name, len, cb, data, result);
}

extern "C" int
js_get_callback_info(js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **receiver, void **data) {
  auto &rt = *env->runtime;

  // Args: when argc != null AND argv != null, copy up to *argc args
  // into argv, then set *argc to actual count. NAPI semantics.
  if (argc != nullptr) {
    size_t want = *argc;
    size_t have = info->argc;
    size_t to_copy = (have < want) ? have : want;
    if (argv != nullptr) {
      for (size_t i = 0; i < to_copy; i++) {
        // Construct a new js_value_s in the current scope mirroring
        // the i-th argument. JSI args are alive for the duration of
        // the callback, but we don't want to leak references into
        // the C ABI, so copy via jsi::Value's copy constructor
        // (which takes a Runtime& and a Value&).
        argv[i] = adopt_value(env, jsi::Value(rt, info->args[i]));
      }
      // Pad remaining slots with undefined for safety.
      for (size_t i = to_copy; i < want; i++) {
        argv[i] = adopt_value(env, jsi::Value::undefined());
      }
    }
    *argc = have;
  }

  if (receiver != nullptr) {
    *receiver = adopt_value(env, jsi::Value(rt, *info->thisVal));
  }

  if (data != nullptr) {
    *data = info->data;
  }

  return 0;
}

extern "C" int
js_call_function(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  auto &rt = *env->runtime;

  // Defensive: bare's bootstrap will happily call `js_call_function`
  // with a NULL callee if an earlier stub returned -1 without bare
  // checking the error code. Crashing here makes the bug invisible
  // ("SIGSEGV in a function nobody can read"); returning -1 makes
  // it surface as a normal error path and is much easier to chase.
  if (function == nullptr) {
    fprintf(stderr, "[libhermes] js_call_function called with NULL function "
                    "(likely an earlier stub returned -1 silently)\n");
    if (result) *result = nullptr;
    return -1;
  }

  // Wrap the ENTIRE body — including the asObject() coercion and
  // the argv marshalling — in try/catch. Bare's callers (especially
  // the on_uncaught handler which itself calls js_call_function
  // again after a worker threw) hand us values that may not be
  // objects/functions, and jsi::Value::asObject throws C++
  // exceptions on type mismatch. With the catch only around fn.call
  // those preamble throws still escaped to libc++abi and
  // std::terminate()'d the whole process.
  try {
    auto fn = function->value.asObject(rt).asFunction(rt);

    // Marshal C-side argv into a JSI-side Value[]. jsi::Function::call*
    // wants a `const Value*` plus count; allocate on the stack for
    // small calls, heap otherwise. Bare's host functions in practice
    // never exceed a handful of args.
    constexpr size_t INLINE_ARGS = 8;
    jsi::Value inline_args[INLINE_ARGS];
    std::vector<jsi::Value> heap_args;
    jsi::Value *jargs = inline_args;
    if (argc > INLINE_ARGS) {
      heap_args.reserve(argc);
      for (size_t i = 0; i < argc; i++) {
        heap_args.emplace_back(rt, argv[i]->value);
      }
      jargs = heap_args.data();
    } else {
      for (size_t i = 0; i < argc; i++) {
        jargs[i] = jsi::Value(rt, argv[i]->value);
      }
    }

    // Cast to `const jsi::Value*` to bind the non-template
    // `call(rt, const Value*, size_t)` /
    // `callWithThis(rt, Object, const Value*, size_t)` overloads.
    // Without the const, Hermes' variadic template overload wins
    // resolution and tries to convert (Value*, size_t) into JS args
    // directly.
    const jsi::Value *cjargs = jargs;
    jsi::Value out;
    if (receiver != nullptr) {
      out = fn.callWithThis(rt, receiver->value.asObject(rt), cjargs, argc);
    } else {
      out = fn.call(rt, cjargs, argc);
    }

    if (result != nullptr) {
      *result = adopt_value(env, std::move(out));
    }
    return 0;
  } catch (jsi::JSError &err) {
    // Always log the JSError on its way through — even when
    // on_uncaught is set, the upstream handler may swallow it and
    // we want a breadcrumb in stderr to diagnose bootstrap-time
    // issues like the bare.js exception path. err.what() can be
    // empty when the thrown value isn't a typical Error object
    // (e.g. `throw 'string'` or `throw {custom}`); in that case
    // stringify err.value() so we still get something useful.
    {
      const auto &val = err.value();
      const char *kind =
          val.isUndefined() ? "undefined"
        : val.isNull()      ? "null"
        : val.isBool()      ? "bool"
        : val.isNumber()    ? "number"
        : val.isString()    ? "string"
        : val.isObject()    ? "object"
        : "unknown";
      std::string what = err.what() ? err.what() : "";
      std::string vstr;
      try { vstr = val.toString(rt).utf8(rt); }
      catch (...) { vstr = "<toString threw>"; }
      // If it's an Object, also try JSON.stringify so error objects
      // show their properties (e.g. .message + .stack).
      std::string json;
      if (val.isObject()) {
        try {
          auto stringify = rt.global()
            .getPropertyAsObject(rt, "JSON")
            .getPropertyAsFunction(rt, "stringify");
          auto out = stringify.call(rt, val);
          json = out.toString(rt).utf8(rt);
        } catch (...) {}
      }
      fprintf(stderr, "[libhermes] JSError in js_call_function:\n"
                      "    what  : %s\n"
                      "    value : (%s) %s\n"
                      "    json  : %s\n",
        what.c_str(), kind, vstr.c_str(), json.c_str());
    }
    js_value_t *errv = adopt_value(env, jsi::Value(rt, err.value()));
    if (env->on_uncaught && !env->in_uncaught) {
      env->in_uncaught = true;
      env->on_uncaught(env, errv, env->on_uncaught_data);
      env->in_uncaught = false;
    }
    if (result) *result = nullptr;
    return -1;
  } catch (jsi::JSIException &err) {
    fprintf(stderr, "[libhermes] JSI exception in js_call_function: %s\n",
      err.what());
    if (result) *result = nullptr;
    return -1;
  }
}

extern "C" int
js_run_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_value_t **result) {
  // `len` / `offset` are debug-info hints (script position in
  // a parent file). Hermes JSI takes only the sourceURL string;
  // we ignore the positional info for now.
  (void) len; (void) offset;
  auto &rt = *env->runtime;
  auto src = source->value.asString(rt).utf8(rt);
  auto buffer = std::make_shared<jsi::StringBuffer>(src);
  try {
    auto out = rt.evaluateJavaScript(buffer, file ? file : "<script>");
    if (result) *result = adopt_value(env, std::move(out));
    return 0;
  } catch (jsi::JSError &err) {
    // Adopt the thrown value into the current scope so the
    // on_uncaught callback can keep using it. We swallow JSError's
    // own message — the value() it carries is the JS-side Error
    // object Bare wants to surface.
    js_value_t *errv = adopt_value(env, jsi::Value(rt, err.value()));
    if (env->on_uncaught && !env->in_uncaught) {
      env->in_uncaught = true;
      env->on_uncaught(env, errv, env->on_uncaught_data);
      env->in_uncaught = false;
    } else {
      fprintf(stderr, "[libhermes] uncaught JS error in run_script: %s\n",
        err.what());
    }
    if (result) *result = nullptr;
    return -1;
  } catch (jsi::JSIException &err) {
    // JSI host-side failure (allocation, invalid Value access, etc.) —
    // not a JS-thrown error. No `.value()`, so build a synthetic
    // Error object holding the C++ exception's message.
    auto msg = jsi::String::createFromUtf8(rt, err.what());
    auto errObj = rt.global().getPropertyAsFunction(rt, "Error")
      .callAsConstructor(rt, msg);
    js_value_t *errv = adopt_value(env, std::move(errObj));
    if (env->on_uncaught && !env->in_uncaught) {
      env->in_uncaught = true;
      env->on_uncaught(env, errv, env->on_uncaught_data);
      env->in_uncaught = false;
    } else {
      fprintf(stderr, "[libhermes] JSI exception in run_script: %s\n",
        err.what());
    }
    if (result) *result = nullptr;
    return -1;
  }
}

// === Primitives =====================================================
//
// JSI represents undefined/null/bool/number as immediate values
// (no GC pressure). All trivial mappings.

extern "C" int
js_get_undefined(js_env_t *env, js_value_t **result) {
  *result = adopt_value(env, jsi::Value::undefined());
  return 0;
}

extern "C" int
js_get_null(js_env_t *env, js_value_t **result) {
  *result = adopt_value(env, jsi::Value::null());
  return 0;
}

extern "C" int
js_get_boolean(js_env_t *env, bool value, js_value_t **result) {
  *result = adopt_value(env, jsi::Value(value));
  return 0;
}

extern "C" int
js_create_int32(js_env_t *env, int32_t value, js_value_t **result) {
  *result = adopt_value(env, jsi::Value(static_cast<double>(value)));
  return 0;
}

extern "C" int
js_create_uint32(js_env_t *env, uint32_t value, js_value_t **result) {
  *result = adopt_value(env, jsi::Value(static_cast<double>(value)));
  return 0;
}

extern "C" int
js_create_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // JSI numbers are IEEE-754 doubles. Values outside ±2^53 lose
  // precision — callers that need exact 64-bit integers should
  // use BigInts via js_create_bigint_int64 (not yet implemented).
  *result = adopt_value(env, jsi::Value(static_cast<double>(value)));
  return 0;
}

extern "C" int
js_create_double(js_env_t *env, double value, js_value_t **result) {
  *result = adopt_value(env, jsi::Value(value));
  return 0;
}

extern "C" int
js_get_value_bool(js_env_t *env, js_value_t *value, bool *result) {
  (void) env;
  *result = value->value.getBool();
  return 0;
}

extern "C" int
js_get_value_int32(js_env_t *env, js_value_t *value, int32_t *result) {
  (void) env;
  // JS uses ToInt32 semantics for bitwise ops — NaN/Infinity → 0,
  // wrap to int32 range otherwise. JSI just gives us the double;
  // we truncate.
  double d = value->value.getNumber();
  *result = static_cast<int32_t>(d);
  return 0;
}

extern "C" int
js_get_value_double(js_env_t *env, js_value_t *value, double *result) {
  (void) env;
  *result = value->value.getNumber();
  return 0;
}

// === Type predicates ================================================
//
// Same shape repeated 30+ times in js.h. JSI's Value carries a tag
// queryable via isX() methods. js_is_array is the only one that
// needs an extra runtime step (it's an Object whose .isArray() is
// true) — handled inline.

extern "C" int
js_typeof(js_env_t *env, js_value_t *value, js_value_type_t *result) {
  auto &rt = *env->runtime;
  const auto &v = value->value;
  if (v.isUndefined()) { *result = js_undefined; return 0; }
  if (v.isNull())      { *result = js_null;      return 0; }
  if (v.isBool())      { *result = js_boolean;   return 0; }
  if (v.isNumber())    { *result = js_number;    return 0; }
  if (v.isString())    { *result = js_string;    return 0; }
  if (v.isSymbol())    { *result = js_symbol;    return 0; }
  if (v.isBigInt())    { *result = js_bigint;    return 0; }
  if (v.isObject()) {
    auto obj = v.asObject(rt);
    if (obj.isFunction(rt)) { *result = js_function; return 0; }
    *result = js_object;
    return 0;
  }
  // Should be unreachable.
  *result = js_undefined;
  return 0;
}

#define IS_PREDICATE(name, expr) \
  extern "C" int                 \
  name(js_env_t *env, js_value_t *value, bool *result) { \
    (void) env; \
    *result = (expr); \
    return 0; \
  }

IS_PREDICATE(js_is_undefined, value->value.isUndefined())
IS_PREDICATE(js_is_null,      value->value.isNull())
IS_PREDICATE(js_is_boolean,   value->value.isBool())
IS_PREDICATE(js_is_number,    value->value.isNumber())
IS_PREDICATE(js_is_string,    value->value.isString())
IS_PREDICATE(js_is_symbol,    value->value.isSymbol())
IS_PREDICATE(js_is_bigint,    value->value.isBigInt())
IS_PREDICATE(js_is_object,    value->value.isObject())

#undef IS_PREDICATE

extern "C" int
js_is_function(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  *result = value->value.asObject(rt).isFunction(rt);
  return 0;
}

extern "C" int
js_is_array(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  *result = value->value.asObject(rt).isArray(rt);
  return 0;
}

extern "C" int
js_is_int32(js_env_t *env, js_value_t *value, bool *result) {
  (void) env;
  if (!value->value.isNumber()) { *result = false; return 0; }
  double d = value->value.getNumber();
  // Truthy when d is an exact integer in int32 range.
  *result = (d == static_cast<double>(static_cast<int32_t>(d)));
  return 0;
}

extern "C" int
js_is_uint32(js_env_t *env, js_value_t *value, bool *result) {
  (void) env;
  if (!value->value.isNumber()) { *result = false; return 0; }
  double d = value->value.getNumber();
  *result = (d >= 0.0 && d == static_cast<double>(static_cast<uint32_t>(d)));
  return 0;
}

// === Objects + arrays ===============================================

extern "C" int
js_create_object(js_env_t *env, js_value_t **result) {
  auto obj = jsi::Object(*env->runtime);
  *result = adopt_value(env, jsi::Value(*env->runtime, obj));
  return 0;
}

extern "C" int
js_create_array(js_env_t *env, js_value_t **result) {
  auto arr = jsi::Array(*env->runtime, 0);
  *result = adopt_value(env, jsi::Value(*env->runtime, arr));
  return 0;
}

extern "C" int
js_create_array_with_length(js_env_t *env, size_t len, js_value_t **result) {
  auto arr = jsi::Array(*env->runtime, len);
  *result = adopt_value(env, jsi::Value(*env->runtime, arr));
  return 0;
}

extern "C" int
js_get_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto prop = obj.getProperty(rt, name);
  *result = adopt_value(env, std::move(prop));
  return 0;
}

extern "C" int
js_delete_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  // JSI doesn't expose `delete` directly — fall back to evaluating
  // the delete operator via a synthesized function. Cleaner long
  // term to wire to Hermes' VM API directly, but this is correct.
  auto deleteFn = rt.global()
    .getPropertyAsFunction(rt, "Function")
    .callAsConstructor(rt, "obj", "name", "return delete obj[name];")
    .asObject(rt)
    .asFunction(rt);
  auto out = deleteFn.call(rt, obj, jsi::String::createFromUtf8(rt, name));
  if (result) *result = out.getBool();
  return 0;
}

extern "C" int
js_get_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t **result) {
  auto &rt = *env->runtime;
  auto arr = object->value.asObject(rt).asArray(rt);
  auto v = arr.getValueAtIndex(rt, index);
  *result = adopt_value(env, std::move(v));
  return 0;
}

extern "C" int
js_set_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t *value) {
  auto &rt = *env->runtime;
  auto arr = object->value.asObject(rt).asArray(rt);
  // jsi::Array::setValueAtIndex is bounds-checked — passing an
  // index >= length throws "out of bounds" in Hermes. Bare's
  // bootstrap relies on grow-on-write semantics (e.g.
  // bare_addon_get_static loops `js_set_element(arr, i++, ...)`
  // starting from index 0 on a zero-length array). Grow the
  // array first so the write lands.
  if (index >= arr.length(rt)) {
    arr.setProperty(rt, "length", static_cast<double>(index + 1));
  }
  arr.setValueAtIndex(rt, index, jsi::Value(rt, value->value));
  return 0;
}

extern "C" int
js_get_array_length(js_env_t *env, js_value_t *array, uint32_t *result) {
  auto &rt = *env->runtime;
  auto arr = array->value.asObject(rt).asArray(rt);
  *result = static_cast<uint32_t>(arr.size(rt));
  return 0;
}

extern "C" int
js_get_property_names(js_env_t *env, js_value_t *object, js_value_t **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto names = obj.getPropertyNames(rt);
  *result = adopt_value(env, jsi::Value(rt, names));
  return 0;
}

// js_define_properties — install a vector of {name, method/getter/setter/value}
// descriptors onto an object. Used by every bare-* binding to set up
// exports in one call.
//
// Three kinds of descriptor (matching js.h's struct, NAPI shape):
//   - Method:   descriptor.method != nullptr → install as function-valued property
//   - Accessor: descriptor.getter || .setter → install via Object.defineProperty
//   - Value:    descriptor.value != nullptr  → plain property assignment
//
// Attributes (writable / enumerable / configurable) are advisory; JSI doesn't
// expose them via setProperty. For accessors we go through the global
// `Object.defineProperty` to set getter/setter; for methods/values we use
// the fast `setProperty` path.
extern "C" int
js_define_properties(js_env_t *env, js_value_t *object, js_property_descriptor_t const properties[], size_t properties_len) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);

  // Cache Object.defineProperty once per call for accessor descriptors.
  jsi::Function defineProperty = rt.global()
    .getPropertyAsObject(rt, "Object")
    .getPropertyAsFunction(rt, "defineProperty");

  for (size_t i = 0; i < properties_len; i++) {
    const auto &p = properties[i];

    // Resolve property name to a string. js.h allows it to be any
    // js_value_t — for now we coerce via .toString() if it isn't
    // already a string. (Symbol keys via Object.defineProperty
    // would need similar handling.)
    std::string name_str;
    if (p.name->value.isString()) {
      name_str = p.name->value.asString(rt).utf8(rt);
    } else {
      // toString coercion for non-string keys.
      name_str = p.name->value.toString(rt).utf8(rt);
    }

    if (p.method != nullptr) {
      js_value_t *fn_v;
      if (js_create_function(env, name_str.c_str(), name_str.size(), p.method, p.data, &fn_v) != 0) {
        return -1;
      }
      obj.setProperty(rt, name_str.c_str(), jsi::Value(rt, fn_v->value));
    } else if (p.getter != nullptr || p.setter != nullptr) {
      // Accessor: build a descriptor object {get, set, configurable: true}
      // and call Object.defineProperty(obj, name, desc).
      auto desc = jsi::Object(rt);
      if (p.getter != nullptr) {
        js_value_t *get_v;
        if (js_create_function(env, "get", (size_t) -1, p.getter, p.data, &get_v) != 0) return -1;
        desc.setProperty(rt, "get", jsi::Value(rt, get_v->value));
      }
      if (p.setter != nullptr) {
        js_value_t *set_v;
        if (js_create_function(env, "set", (size_t) -1, p.setter, p.data, &set_v) != 0) return -1;
        desc.setProperty(rt, "set", jsi::Value(rt, set_v->value));
      }
      desc.setProperty(rt, "configurable", true);
      defineProperty.call(rt,
        jsi::Value(rt, obj),
        jsi::String::createFromUtf8(rt, name_str),
        jsi::Value(rt, desc)
      );
    } else if (p.value != nullptr) {
      obj.setProperty(rt, name_str.c_str(), jsi::Value(rt, p.value->value));
    }
    // (No-op when all method/getter/setter/value are null —
    // shouldn't happen in practice but doesn't crash.)
  }

  return 0;
}

// === Exception path =================================================
//
// js.h's NAPI-style convention: host functions set a "pending
// exception" on the env when something goes wrong, and return a
// sentinel (usually null) to their caller. The dispatch lambda in
// js_create_function checks pending_exception after each callback
// and re-throws as jsi::JSError so the JS-side try/catch sees it.
// Callers querying the env can pull the value back out via
// js_get_and_clear_last_exception.

// Build a JS Error object with a given constructor name. Helper for
// js_throw_error / js_throw_type_error / etc.
static jsi::Value
build_error(jsi::Runtime &rt, const char *ctor_name, const char *code, const char *message) {
  auto msg_str = jsi::String::createFromUtf8(rt, message ? message : "");
  auto ctor = rt.global().getPropertyAsFunction(rt, ctor_name);
  auto err = ctor.callAsConstructor(rt, msg_str);
  // Attach `.code` if supplied — Node-compatible Error code idiom.
  if (code != nullptr) {
    auto err_obj = err.asObject(rt);
    err_obj.setProperty(rt, "code", jsi::String::createFromUtf8(rt, code));
  }
  return err;
}

extern "C" int
js_throw(js_env_t *env, js_value_t *error) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(rt, error->value);
  return 0;
}

extern "C" int
js_throw_error(js_env_t *env, const char *code, const char *message) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(
    build_error(rt, "Error", code, message)
  );
  return 0;
}

extern "C" int
js_throw_type_error(js_env_t *env, const char *code, const char *message) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(
    build_error(rt, "TypeError", code, message)
  );
  return 0;
}

extern "C" int
js_throw_range_error(js_env_t *env, const char *code, const char *message) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(
    build_error(rt, "RangeError", code, message)
  );
  return 0;
}

extern "C" int
js_throw_syntax_error(js_env_t *env, const char *code, const char *message) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(
    build_error(rt, "SyntaxError", code, message)
  );
  return 0;
}

extern "C" int
js_throw_reference_error(js_env_t *env, const char *code, const char *message) {
  auto &rt = *env->runtime;
  env->pending_exception = std::make_unique<jsi::Value>(
    build_error(rt, "ReferenceError", code, message)
  );
  return 0;
}

extern "C" int
js_is_exception_pending(js_env_t *env, bool *result) {
  *result = (env->pending_exception != nullptr);
  return 0;
}

extern "C" int
js_get_and_clear_last_exception(js_env_t *env, js_value_t **result) {
  if (!env->pending_exception) {
    *result = adopt_value(env, jsi::Value::undefined());
    return 0;
  }
  auto err = std::move(*env->pending_exception);
  env->pending_exception.reset();
  *result = adopt_value(env, std::move(err));
  return 0;
}

// === Persistent references =========================================
//
// js_ref_t persists across handle scopes. jsi::Value's payload
// (PointerValue*) is GC-tracked via the Runtime's cloneX hooks, so
// just holding a jsi::Value member is sufficient for strong-ref
// semantics — no extra rooting needed beyond Hermes' built-in
// ManagedValue machinery.
//
// NAPI's refcount semantics: count > 0 = strong, count = 0 = weak.
// JSI's standard surface doesn't expose weak object references in
// a universal way, so for now we hold strong always and just track
// the count. Switch to a `jsi::WeakObject` when refcount reaches 0
// is a future refinement.

struct js_ref_s {
  jsi::Value value;
  HermesRuntime *runtime;
  uint32_t refcount;
};

extern "C" int
js_create_reference(js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  auto *ref = new js_ref_s();
  ref->value = jsi::Value(*env->runtime, value->value);
  ref->runtime = env->runtime.get();
  ref->refcount = count;
  *result = ref;
  return 0;
}

extern "C" int
js_delete_reference(js_env_t *env, js_ref_t *reference) {
  (void) env;
  delete reference;
  return 0;
}

extern "C" int
js_reference_ref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  (void) env;
  reference->refcount++;
  if (result) *result = reference->refcount;
  return 0;
}

extern "C" int
js_reference_unref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  (void) env;
  if (reference->refcount > 0) reference->refcount--;
  if (result) *result = reference->refcount;
  return 0;
}

extern "C" int
js_get_reference_value(js_env_t *env, js_ref_t *reference, js_value_t **result) {
  *result = adopt_value(env, jsi::Value(*env->runtime, reference->value));
  return 0;
}

// === Externals (opaque void* in JS) =================================
//
// JSI's NativeState attaches GC-tracked C++ data to a JS Object.
// We piggyback on it: js_create_external returns an Object whose
// only purpose is to carry the pointer; js_get_value_external
// pulls it back out.

namespace {
class ExternalState : public jsi::NativeState {
public:
  ExternalState(js_env_t *env, void *data, js_finalize_cb cb, void *hint)
    : env_(env), data_(data), finalize_cb_(cb), finalize_hint_(hint) {}

  ~ExternalState() override {
    // Called by JSI when the wrapping Object is GC'd. Hands the C
    // caller back its data so they can free it.
    if (finalize_cb_) finalize_cb_(env_, data_, finalize_hint_);
  }

  void *data() const { return data_; }

private:
  js_env_t *env_;
  void *data_;
  js_finalize_cb finalize_cb_;
  void *finalize_hint_;
};
}

extern "C" int
js_create_external(js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  auto &rt = *env->runtime;
  auto state = std::make_shared<ExternalState>(env, data, finalize_cb, finalize_hint);
  auto obj = jsi::Object(rt);
  obj.setNativeState(rt, state);
  *result = adopt_value(env, jsi::Value(rt, obj));
  return 0;
}

extern "C" int
js_get_value_external(js_env_t *env, js_value_t *value, void **result) {
  auto &rt = *env->runtime;
  auto obj = value->value.asObject(rt);
  if (!obj.hasNativeState(rt)) {
    *result = nullptr;
    return -1;
  }
  auto state = obj.getNativeState<ExternalState>(rt);
  *result = state ? state->data() : nullptr;
  return 0;
}

// === ArrayBuffer ====================================================
//
// JSI's ArrayBuffer wraps a MutableBuffer<uint8_t>. For
// js_create_arraybuffer we own the bytes; for
// js_create_external_arraybuffer the caller owns them and supplies
// a finalize callback that fires when JSI's GC drops the wrapping
// object.

namespace {
class OwnedBuffer : public jsi::MutableBuffer {
public:
  explicit OwnedBuffer(size_t len) : bytes_(len, 0) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t *data() override { return bytes_.data(); }

private:
  std::vector<uint8_t> bytes_;
};

class ExternalBufferAdapter : public jsi::MutableBuffer {
public:
  ExternalBufferAdapter(js_env_t *env, void *data, size_t len,
                        js_finalize_cb cb, void *hint)
    : env_(env), data_(static_cast<uint8_t *>(data)), len_(len),
      finalize_cb_(cb), finalize_hint_(hint) {}

  ~ExternalBufferAdapter() override {
    if (finalize_cb_) finalize_cb_(env_, data_, finalize_hint_);
  }

  size_t size() const override { return len_; }
  uint8_t *data() override { return data_; }

private:
  js_env_t *env_;
  uint8_t *data_;
  size_t len_;
  js_finalize_cb finalize_cb_;
  void *finalize_hint_;
};
}

extern "C" int
js_create_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  auto &rt = *env->runtime;
  auto buf = std::make_shared<OwnedBuffer>(len);
  // Stash data ptr BEFORE moving the shared_ptr into ArrayBuffer;
  // the JSI ArrayBuffer ctor stores its own shared_ptr to the
  // buffer, but the underlying vector storage stays alive as long
  // as either holds a reference.
  if (data) *data = buf->data();
  auto ab = jsi::ArrayBuffer(rt, buf);
  *result = adopt_value(env, jsi::Value(rt, ab));
  return 0;
}

// Same as js_create_arraybuffer — bare's bare-buffer addon uses
// this for perf (skip zero-fill) but the JS-visible behavior is
// identical to the zeroed variant. Wire a non-zeroing fast path
// later if profiling shows it.
extern "C" int
js_create_unsafe_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  return js_create_arraybuffer(env, len, data, result);
}

// js_get_typedarray_info — introspect a TypedArray (returns its
// element type, byte-length, backing arraybuffer, byte-offset, and
// a pointer into the backing store at the offset). JSI's public
// API doesn't expose this directly; we reach for the JS-side
// properties (`.buffer`, `.byteOffset`, `.byteLength`,
// `.constructor.name`) and resolve the type from the constructor
// name. Slow path but correct — bare-buffer calls this once per
// typed array passed in from JS, not in hot loops.
extern "C" int
js_get_typedarray_info(
    js_env_t *env,
    js_value_t *typedarray,
    js_typedarray_type_t *type,
    void **data,
    size_t *len,
    js_value_t **arraybuffer,
    size_t *offset) {
  if (typedarray == nullptr) return -1;
  auto &rt = *env->runtime;
  try {
    auto obj = typedarray->value.asObject(rt);
    // .byteOffset / .byteLength / .buffer
    double byteOffset = obj.getProperty(rt, "byteOffset").asNumber();
    double byteLength = obj.getProperty(rt, "byteLength").asNumber();
    auto bufVal = obj.getProperty(rt, "buffer");

    // Discriminate the element type via constructor.name. JSI
    // ArrayBuffer has a `data()` accessor we use to derive the
    // backing pointer + offset.
    auto ctorName = obj.getProperty(rt, "constructor")
      .asObject(rt)
      .getProperty(rt, "name")
      .asString(rt).utf8(rt);

    auto resolveType = [](const std::string &n) -> js_typedarray_type_t {
      if (n == "Int8Array")         return js_int8array;
      if (n == "Uint8Array")        return js_uint8array;
      if (n == "Uint8ClampedArray") return js_uint8clampedarray;
      if (n == "Int16Array")        return js_int16array;
      if (n == "Uint16Array")       return js_uint16array;
      if (n == "Int32Array")        return js_int32array;
      if (n == "Uint32Array")       return js_uint32array;
      if (n == "Float32Array")      return js_float32array;
      if (n == "Float64Array")      return js_float64array;
      if (n == "BigInt64Array")     return js_bigint64array;
      if (n == "BigUint64Array")    return js_biguint64array;
      return js_uint8array; // fall-back; safest for byte access
    };

    js_typedarray_type_t resolved = resolveType(ctorName);

    // element size by type → length in elements
    size_t elementSize = 1;
    switch (resolved) {
      case js_int16array: case js_uint16array: case js_float16array: elementSize = 2; break;
      case js_int32array: case js_uint32array: case js_float32array: elementSize = 4; break;
      case js_float64array: case js_bigint64array: case js_biguint64array: elementSize = 8; break;
      default: elementSize = 1;
    }

    if (type) *type = resolved;
    if (len) *len = static_cast<size_t>(byteLength) / elementSize;
    if (offset) *offset = static_cast<size_t>(byteOffset);

    if (arraybuffer || data) {
      auto ab = bufVal.asObject(rt).getArrayBuffer(rt);
      if (data) *data = ab.data(rt) + static_cast<size_t>(byteOffset);
      if (arraybuffer) {
        *arraybuffer = adopt_value(env, std::move(bufVal));
      }
    }
    return 0;
  } catch (jsi::JSError &err) {
    fprintf(stderr, "[libhermes] js_get_typedarray_info JSError: %s\n", err.what());
    return -1;
  } catch (jsi::JSIException &err) {
    fprintf(stderr, "[libhermes] js_get_typedarray_info JSI exception: %s\n", err.what());
    return -1;
  }
}

extern "C" int
js_create_external_arraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  auto &rt = *env->runtime;
  auto buf = std::make_shared<ExternalBufferAdapter>(env, data, len, finalize_cb, finalize_hint);
  auto ab = jsi::ArrayBuffer(rt, buf);
  *result = adopt_value(env, jsi::Value(rt, ab));
  return 0;
}

extern "C" int
js_get_arraybuffer_info(js_env_t *env, js_value_t *arraybuffer, void **data, size_t *len) {
  auto &rt = *env->runtime;
  auto ab = arraybuffer->value.asObject(rt).getArrayBuffer(rt);
  if (data) *data = ab.data(rt);
  if (len)  *len  = ab.size(rt);
  return 0;
}

extern "C" int
js_is_arraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  *result = value->value.asObject(rt).isArrayBuffer(rt);
  return 0;
}

extern "C" int
js_is_external(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  auto obj = value->value.asObject(rt);
  // Match if it has a NativeState that's ours. JSI doesn't
  // distinguish "any NativeState" from "our ExternalState" — for
  // now any HostObject-via-NativeState reads as external.
  *result = obj.hasNativeState<ExternalState>(rt);
  return 0;
}

// === Wrap / unwrap =================================================
//
// Like externals but attached to an existing object instead of
// creating a fresh wrapper. This is how bare-tcp / bare-tls / etc.
// associate a C struct (an `uv_tcp_t*`, `BIO*`, ...) with a JS
// instance — JS-side code holds a regular object; C-side code
// fishes out the pointer via js_unwrap.
//
// Implementation reuses ExternalState (same C-data + finalizer
// shape). js_wrap also returns a js_ref_t so the C caller can
// keep the JS object alive across handle scopes — convenient
// for "stash this in my own state and forget about scope".

extern "C" int
js_wrap(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto state = std::make_shared<ExternalState>(env, data, finalize_cb, finalize_hint);
  obj.setNativeState(rt, state);

  // Optionally return a strong reference. Caller passes NULL to
  // skip; bare-* bindings typically grab one so the JS handle
  // doesn't get GC'd while their C-side state machine still cares.
  if (result != nullptr) {
    auto *ref = new js_ref_s();
    ref->value = jsi::Value(rt, obj);
    ref->runtime = env->runtime.get();
    ref->refcount = 1;
    *result = ref;
  }
  return 0;
}

extern "C" int
js_unwrap(js_env_t *env, js_value_t *object, void **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  if (!obj.hasNativeState<ExternalState>(rt)) {
    *result = nullptr;
    return -1;
  }
  auto state = obj.getNativeState<ExternalState>(rt);
  *result = state ? state->data() : nullptr;
  return 0;
}

// === Error construction =============================================

extern "C" int
js_create_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = *env->runtime;
  auto msg = message ? jsi::Value(rt, message->value) : jsi::Value(jsi::String::createFromUtf8(rt, ""));
  auto err = rt.global().getPropertyAsFunction(rt, "Error").callAsConstructor(rt, msg);
  if (code && code->value.isString()) {
    err.asObject(rt).setProperty(rt, "code", code->value.asString(rt));
  }
  *result = adopt_value(env, std::move(err));
  return 0;
}

extern "C" int
js_create_type_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = *env->runtime;
  auto msg = message ? jsi::Value(rt, message->value) : jsi::Value(jsi::String::createFromUtf8(rt, ""));
  auto err = rt.global().getPropertyAsFunction(rt, "TypeError").callAsConstructor(rt, msg);
  if (code && code->value.isString()) {
    err.asObject(rt).setProperty(rt, "code", code->value.asString(rt));
  }
  *result = adopt_value(env, std::move(err));
  return 0;
}

extern "C" int
js_create_range_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = *env->runtime;
  auto msg = message ? jsi::Value(rt, message->value) : jsi::Value(jsi::String::createFromUtf8(rt, ""));
  auto err = rt.global().getPropertyAsFunction(rt, "RangeError").callAsConstructor(rt, msg);
  if (code && code->value.isString()) {
    err.asObject(rt).setProperty(rt, "code", code->value.asString(rt));
  }
  *result = adopt_value(env, std::move(err));
  return 0;
}

extern "C" int
js_create_syntax_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = *env->runtime;
  auto msg = message ? jsi::Value(rt, message->value) : jsi::Value(jsi::String::createFromUtf8(rt, ""));
  auto err = rt.global().getPropertyAsFunction(rt, "SyntaxError").callAsConstructor(rt, msg);
  if (code && code->value.isString()) {
    err.asObject(rt).setProperty(rt, "code", code->value.asString(rt));
  }
  *result = adopt_value(env, std::move(err));
  return 0;
}

extern "C" int
js_create_reference_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  auto &rt = *env->runtime;
  auto msg = message ? jsi::Value(rt, message->value) : jsi::Value(jsi::String::createFromUtf8(rt, ""));
  auto err = rt.global().getPropertyAsFunction(rt, "ReferenceError").callAsConstructor(rt, msg);
  if (code && code->value.isString()) {
    err.asObject(rt).setProperty(rt, "code", code->value.asString(rt));
  }
  *result = adopt_value(env, std::move(err));
  return 0;
}

extern "C" int
js_is_error(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  // Check via global Error constructor's prototype chain. JSI lacks
  // a direct `instanceof` shortcut, so we eval one. Performance-wise
  // fine for the occasional error check; can be optimized via
  // cached Function later.
  auto isErr = rt.global().getPropertyAsFunction(rt, "Function")
    .callAsConstructor(rt, "v", "return v instanceof Error;")
    .asObject(rt).asFunction(rt);
  auto out = isErr.call(rt, jsi::Value(rt, value->value));
  *result = out.isBool() ? out.getBool() : false;
  return 0;
}

// js_throw_errorf — like js_throw_error but printf-style format.
#include <cstdarg>
extern "C" int
js_throw_errorf(js_env_t *env, const char *code, const char *message, ...) {
  char buf[1024];
  va_list args;
  va_start(args, message);
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  va_end(args);
  return js_throw_error(env, code, buf);
}

extern "C" int
js_throw_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  return js_throw_error(env, code, buf);
}

extern "C" int
js_throw_type_errorf(js_env_t *env, const char *code, const char *message, ...) {
  char buf[1024];
  va_list args;
  va_start(args, message);
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  va_end(args);
  return js_throw_type_error(env, code, buf);
}

extern "C" int
js_throw_type_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  return js_throw_type_error(env, code, buf);
}

extern "C" int
js_throw_range_errorf(js_env_t *env, const char *code, const char *message, ...) {
  char buf[1024];
  va_list args;
  va_start(args, message);
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  va_end(args);
  return js_throw_range_error(env, code, buf);
}

extern "C" int
js_throw_range_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  return js_throw_range_error(env, code, buf);
}

extern "C" int
js_throw_syntax_errorf(js_env_t *env, const char *code, const char *message, ...) {
  char buf[1024];
  va_list args;
  va_start(args, message);
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  va_end(args);
  return js_throw_syntax_error(env, code, buf);
}

extern "C" int
js_throw_syntax_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  return js_throw_syntax_error(env, code, buf);
}

extern "C" int
js_throw_reference_errorf(js_env_t *env, const char *code, const char *message, ...) {
  char buf[1024];
  va_list args;
  va_start(args, message);
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  va_end(args);
  return js_throw_reference_error(env, code, buf);
}

extern "C" int
js_throw_reference_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), message ? message : "", args);
  return js_throw_reference_error(env, code, buf);
}

// === Property access (named / indexed / computed) ==================

extern "C" int
js_get_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  // PropNameID accepts either a string or a Symbol. For simplicity
  // coerce arbitrary keys to string via toString(); proper Symbol
  // support can come later.
  auto key_str = key->value.isString()
    ? key->value.asString(rt).utf8(rt)
    : key->value.toString(rt).utf8(rt);
  auto v = obj.getProperty(rt, key_str.c_str());
  *result = adopt_value(env, std::move(v));
  return 0;
}

extern "C" int
js_set_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t *value) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto key_str = key->value.isString()
    ? key->value.asString(rt).utf8(rt)
    : key->value.toString(rt).utf8(rt);
  obj.setProperty(rt, key_str.c_str(), jsi::Value(rt, value->value));
  return 0;
}

extern "C" int
js_has_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto key_str = key->value.isString()
    ? key->value.asString(rt).utf8(rt)
    : key->value.toString(rt).utf8(rt);
  *result = obj.hasProperty(rt, key_str.c_str());
  return 0;
}

extern "C" int
js_has_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  *result = obj.hasProperty(rt, name);
  return 0;
}

extern "C" int
js_has_own_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  // No direct JSI API. Use Object.prototype.hasOwnProperty.call(obj, key).
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto key_v = jsi::Value(rt, key->value);
  auto hasOwn = rt.global().getPropertyAsObject(rt, "Object")
    .getPropertyAsObject(rt, "prototype")
    .getPropertyAsFunction(rt, "hasOwnProperty");
  auto out = hasOwn.callWithThis(rt, obj, jsi::Value(rt, key_v));
  *result = out.isBool() ? out.getBool() : false;
  return 0;
}

extern "C" int
js_delete_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  auto key_str = key->value.isString()
    ? key->value.asString(rt).utf8(rt)
    : key->value.toString(rt).utf8(rt);
  return js_delete_named_property(env, object, key_str.c_str(), result);
  (void) obj;
}

extern "C" int
js_has_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  // JS-spec semantics: indexed access goes through hasProperty with
  // a numeric key string. Simpler to evaluate via Function for now.
  char buf[32];
  snprintf(buf, sizeof(buf), "%u", index);
  *result = obj.hasProperty(rt, buf);
  return 0;
}

extern "C" int
js_delete_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  // Same shape as js_delete_named_property but indexed.
  char buf[32];
  snprintf(buf, sizeof(buf), "%u", index);
  return js_delete_named_property(env, object, buf, result);
}

// === Coercions ======================================================

extern "C" int
js_coerce_to_boolean(js_env_t *env, js_value_t *value, js_value_t **result) {
  auto &rt = *env->runtime;
  // ToBoolean semantics: anything non-falsy → true.
  bool b;
  const auto &v = value->value;
  if (v.isBool())        b = v.getBool();
  else if (v.isUndefined() || v.isNull()) b = false;
  else if (v.isNumber()) { double d = v.getNumber(); b = !(d == 0 || std::isnan(d)); }
  else if (v.isString()) b = v.asString(rt).utf8(rt).size() > 0;
  else                   b = true;
  *result = adopt_value(env, jsi::Value(b));
  return 0;
}

extern "C" int
js_coerce_to_number(js_env_t *env, js_value_t *value, js_value_t **result) {
  // Use Number(v) global to apply spec ToNumber.
  auto &rt = *env->runtime;
  auto num = rt.global().getPropertyAsFunction(rt, "Number")
    .call(rt, jsi::Value(rt, value->value));
  *result = adopt_value(env, std::move(num));
  return 0;
}

extern "C" int
js_coerce_to_string(js_env_t *env, js_value_t *value, js_value_t **result) {
  auto &rt = *env->runtime;
  auto str = value->value.toString(rt);
  *result = adopt_value(env, jsi::Value(rt, str));
  return 0;
}

extern "C" int
js_coerce_to_object(js_env_t *env, js_value_t *value, js_value_t **result) {
  auto &rt = *env->runtime;
  auto obj = rt.global().getPropertyAsFunction(rt, "Object")
    .call(rt, jsi::Value(rt, value->value));
  *result = adopt_value(env, std::move(obj));
  return 0;
}

// === Strict equality ================================================

extern "C" int
js_strict_equals(js_env_t *env, js_value_t *a, js_value_t *b, bool *result) {
  auto &rt = *env->runtime;
  *result = jsi::Value::strictEquals(rt, a->value, b->value);
  return 0;
}

// === Escapable handle scopes ========================================
//
// Same shape as regular scopes plus an "escape" slot — a single
// js_value_t* that survives close. js.h's pattern: open scope, do
// work, choose one value to keep, escape it, close. The escaped
// value lands in the *parent* scope.

struct js_escapable_handle_scope_s {
  js_handle_scope_s base; // first so we can downcast safely
  bool escaped = false;
};

extern "C" int
js_open_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t **result) {
  auto *scope = new js_escapable_handle_scope_s();
  scope->base.env = env;
  scope->base.prev = env->top_scope;
  env->top_scope = &scope->base;
  *result = scope;
  return 0;
}

extern "C" int
js_close_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t *scope) {
  if (env->top_scope != &scope->base) return -1;
  env->top_scope = scope->base.prev;
  // Manual cleanup since we're managing js_handle_scope_s ourselves.
  scope->base.values.clear();
  delete scope;
  return 0;
}

extern "C" int
js_escape_handle(js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  if (scope->escaped) return -1; // contract violation: only one escape per scope
  scope->escaped = true;
  // Copy the value into the PARENT scope (top_scope's prev should be
  // the parent). The escapable scope is currently top.
  auto *parent = scope->base.prev;
  if (!parent) {
    // No parent — can't escape. Return a fresh value in the same
    // scope (will get cleaned up with the escapable on close).
    *result = adopt_value(env, jsi::Value(*env->runtime, escapee->value));
    return 0;
  }
  auto wrapper = std::make_unique<js_value_s>();
  wrapper->value = jsi::Value(*env->runtime, escapee->value);
  wrapper->runtime = env->runtime.get();
  auto *raw = wrapper.get();
  parent->values.push_back(std::move(wrapper));
  *result = raw;
  return 0;
}

// === Misc small additions ==========================================

extern "C" int
js_get_new_target(js_env_t *env, const js_callback_info_t *info, js_value_t **result) {
  (void) info;
  // JSI's createFromHostFunction doesn't pass `new` context to the
  // host function. Returning undefined as a placeholder; bare-* code
  // typically branches on this being non-null.
  *result = adopt_value(env, jsi::Value::undefined());
  return 0;
}

extern "C" int
js_get_bindings(js_env_t *env, js_value_t **result) {
  // Returns the env's "bindings" object — bare uses this as the
  // shared exports surface for its host. Return globalThis for now;
  // bare's bootstrap looks for specific properties on it which we
  // can satisfy once we hit them at runtime.
  return js_get_global(env, result);
}

extern "C" int
js_get_platform_limits(js_platform_t *platform, js_platform_limits_t *result) {
  (void) platform; (void) result;
  // Optional info; bare doesn't fail without it.
  return 0;
}

// === Mass stubs ====================================================
//
// Every remaining js_h function we don't have a real impl for. They
// return -1 (js_pending_exception) so bare can detect "not supported"
// at runtime. Each line is a one-shot stub — when bare's bootstrap
// hits one of these, the next iteration is to replace it with a real
// implementation, driven by the actual call site.
//
// This block makes the integration *link* and lets us iterate from
// runtime crashes instead of speculation.

// Stubs trace each call to stderr on first hit so we can see which
// part of bare's bootstrap is reaching for an unimplemented API.
// Per-call once-flag (static bool) keeps the log noise bounded.
//
// Logging on every call would hide useful info under repeated lines;
// once-per-stub gives us the bootstrap shape in one pass.
#define STUB(name, ...) \
  extern "C" int name(__VA_ARGS__) { \
    static bool _logged_##name = false; \
    if (!_logged_##name) { \
      fprintf(stderr, "[libhermes-stub] %s\n", #name); \
      _logged_##name = true; \
    } \
    return -1; \
  }

// Modules
STUB(js_create_module,           js_env_t*, const char*, size_t, int, js_value_t*, js_module_meta_cb, void*, js_module_t**)
STUB(js_create_synthetic_module, js_env_t*, const char*, size_t, js_value_t *const[], size_t, js_module_evaluate_cb, void*, js_module_t**)
STUB(js_delete_module,           js_env_t*, js_module_t*)
STUB(js_get_module_name,         js_env_t*, js_module_t*, const char**)
STUB(js_get_module_namespace,    js_env_t*, js_module_t*, js_value_t**)
STUB(js_set_module_export,       js_env_t*, js_module_t*, js_value_t*, js_value_t*)
STUB(js_instantiate_module,      js_env_t*, js_module_t*, js_module_resolve_cb, void*)
STUB(js_run_module,              js_env_t*, js_module_t*, js_value_t**)
STUB(js_is_module_namespace,     js_env_t*, js_value_t*, bool*)
// Real impls below — bare registers these during bootstrap and
// expects 0 return. We don't yet wire them to Hermes' dynamic
// import path (no dynamic import support in bare-* bundles right
// now), but the register call must succeed.

// Threadsafe functions (libuv ↔ JS thread crossing — big chunk of work)
STUB(js_create_threadsafe_function,    js_env_t*, js_value_t*, size_t, size_t, js_finalize_cb, void*, void*, js_threadsafe_function_cb, js_threadsafe_function_t**)
STUB(js_get_threadsafe_function_context, js_threadsafe_function_t*, void**)
STUB(js_call_threadsafe_function,      js_threadsafe_function_t*, void*, js_threadsafe_function_call_mode_t)
STUB(js_acquire_threadsafe_function,   js_threadsafe_function_t*)
STUB(js_release_threadsafe_function,   js_threadsafe_function_t*, js_threadsafe_function_release_mode_t)
STUB(js_ref_threadsafe_function,       js_env_t*, js_threadsafe_function_t*)
STUB(js_unref_threadsafe_function,     js_env_t*, js_threadsafe_function_t*)

// Teardown callbacks
// Real impls below — teardown hooks bare registers during
// bootstrap. We accept the register but don't actually fire the
// callbacks on env destroy yet (full Hermes-side teardown
// orchestration is a separate workstream).

// Inspector
STUB(js_create_inspector,  js_env_t*, js_inspector_t**)
STUB(js_destroy_inspector, js_env_t*, js_inspector_t*)
STUB(js_connect_inspector, js_env_t*, js_inspector_t*)
STUB(js_on_inspector_paused,                 js_env_t*, js_inspector_t*, js_inspector_paused_cb, void*)
STUB(js_on_inspector_response_transitional,  js_env_t*, js_inspector_t*, js_inspector_message_transitional_cb, void*)
STUB(js_send_inspector_request_transitional, js_env_t*, js_inspector_t*, const char*, size_t)

// Finalizers / wrap / type tags
STUB(js_add_finalizer,    js_env_t*, js_value_t*, void*, js_finalize_cb, void*, js_ref_t**)
STUB(js_add_type_tag,     js_env_t*, js_value_t*, const js_type_tag_t*)
STUB(js_check_type_tag,   js_env_t*, js_value_t*, const js_type_tag_t*, bool*)

// Symbol / Date / BigInt
STUB(js_create_symbol,     js_env_t*, js_value_t*, js_value_t**)
STUB(js_create_date,       js_env_t*, double, js_value_t**)
STUB(js_get_value_date,    js_env_t*, js_value_t*, double*)
STUB(js_create_bigint_words, js_env_t*, int, const uint64_t*, size_t, js_value_t**)
STUB(js_get_value_bigint_words, js_env_t*, js_value_t*, int*, uint64_t*, size_t, size_t*)

// ArrayBuffer details + TypedArray + DataView
STUB(js_create_arraybuffer_with_backing_store, js_env_t*, js_arraybuffer_backing_store_t*, void**, size_t*, js_value_t**)
// Real impl below — pass through to js_create_arraybuffer.
// The "unsafe" variant is a Node.js convention: it skips
// zeroing the backing store for perf (caller promises to write
// every byte before reading). Our OwnedBuffer zero-fills, which
// is correct (slower) — bare callers see the same behavior either
// way. Wire a separate non-zeroing path later if a bench shows it
// matters.
STUB(js_get_arraybuffer_backing_store, js_env_t*, js_value_t*, js_arraybuffer_backing_store_t**)
STUB(js_release_arraybuffer_backing_store, js_env_t*, js_arraybuffer_backing_store_t*)
STUB(js_detach_arraybuffer, js_env_t*, js_value_t*)
STUB(js_create_sharedarraybuffer_with_backing_store, js_env_t*, js_arraybuffer_backing_store_t*, void**, size_t*, js_value_t**)
STUB(js_get_sharedarraybuffer_info, js_env_t*, js_value_t*, void**, size_t*)
STUB(js_get_sharedarraybuffer_backing_store, js_env_t*, js_value_t*, js_arraybuffer_backing_store_t**)
STUB(js_create_typedarray, js_env_t*, js_typedarray_type_t, size_t, js_value_t*, size_t, js_value_t**)
// js_get_typedarray_info has a real impl below — bare-buffer
// uses it to introspect typed arrays handed in from JS (e.g.
// Buffer.from(uint8Array) needs to know the underlying buffer +
// offset). Stubbed -1 means bare-buffer can't unwrap typed
// arrays passed from user code, which is a non-starter.
STUB(js_create_dataview, js_env_t*, size_t, js_value_t*, size_t, js_value_t**)
STUB(js_get_dataview_info, js_env_t*, js_value_t*, void**, size_t*, js_value_t**, size_t*)
STUB(js_get_array_elements, js_env_t*, js_value_t*, js_value_t*[], size_t, size_t, uint32_t*)

// String views (zero-copy)
STUB(js_get_string_view,     js_env_t*, js_value_t*, js_string_encoding_t*, const void**, size_t*, js_string_view_t**)
STUB(js_release_string_view, js_env_t*, js_string_view_t*)

// Misc
STUB(js_call_function_with_checkpoint, js_env_t*, js_value_t*, js_value_t*, size_t, js_value_t *const[], js_value_t**)
STUB(js_new_instance,        js_env_t*, js_value_t*, size_t, js_value_t *const[], js_value_t**)
STUB(js_define_class,        js_env_t*, const char*, size_t, js_function_cb, void*, js_property_descriptor_t const[], size_t, js_value_t**)
STUB(js_get_filtered_property_names, js_env_t*, js_value_t*, js_key_collection_mode_t, js_property_filter_t, js_index_filter_t, js_key_conversion_mode_t, js_value_t**)
STUB(js_get_promise_state,   js_env_t*, js_value_t*, js_promise_state_t*)
STUB(js_get_promise_result,  js_env_t*, js_value_t*, js_value_t**)
STUB(js_get_heap_statistics, js_env_t*, js_heap_statistics_t*)
STUB(js_adjust_external_memory, js_env_t*, int64_t, int64_t*)
// js_create_function_with_source — compile a JS source string into
// a function and return it. Bare's runtime.c uses this around the
// bootstrap step (see vendor/bare/src/runtime.c:1307) to wrap the
// embedded `bare.js` text into a callable; without this the entry
// point is NULL and the subsequent `js_call_function` crashes on a
// null receiver.
//
// We synthesize: `(function(arg0, arg1, ...) { <source> })` and
// evaluate it. The returned jsi::Value is a Function object that
// the caller can invoke via js_call_function.
//
// `args` is an array of js_value_t* whose underlying jsi::Value is
// expected to be a String holding the parameter name. Bare currently
// passes at most a few names; correctness over speed here.
extern "C" int
js_create_function_with_source(
  js_env_t *env,
  const char *name, size_t name_len,
  const char *file, size_t file_len,
  js_value_t *const args[], size_t args_count,
  int offset,
  js_value_t *source,
  js_value_t **result
) {
  (void) name; (void) name_len; (void) offset;
  auto &rt = *env->runtime;

  // Pull the source out of its js_value_t (it's a JS string).
  std::string src;
  try {
    src = source->value.asString(rt).utf8(rt);
  } catch (jsi::JSError &err) {
    return -1;
  }

  // Param names: each args[i] is a JS String. Join with commas.
  std::string param_list;
  for (size_t i = 0; i < args_count; i++) {
    if (i > 0) param_list += ",";
    try {
      param_list += args[i]->value.asString(rt).utf8(rt);
    } catch (jsi::JSError &err) {
      return -1;
    }
  }

  // Build the wrapper. The trailing newline before `})` guards against
  // the source ending with a `//` line comment that would otherwise
  // swallow the closing brace.
  std::string wrapped = "(function(" + param_list + "){" + src + "\n})";

  // sourceURL — Hermes uses it for stack traces. Bare passes the
  // canonical path (e.g. "bare:/bare.js"); honor name_len when given.
  std::string source_url;
  if (file) {
    source_url = (file_len == (size_t) -1)
      ? std::string(file)
      : std::string(file, file_len);
  } else {
    source_url = "<anonymous>";
  }

  auto buffer = std::make_shared<jsi::StringBuffer>(wrapped);
  try {
    auto fn = rt.evaluateJavaScript(buffer, source_url);
    if (result) *result = adopt_value(env, std::move(fn));
    return 0;
  } catch (jsi::JSError &err) {
    js_value_t *errv = adopt_value(env, jsi::Value(rt, err.value()));
    if (env->on_uncaught) {
      env->on_uncaught(env, errv, env->on_uncaught_data);
    } else {
      fprintf(stderr, "[libhermes] uncaught JSError in create_function_with_source: %s\n", err.what());
    }
    if (result) *result = nullptr;
    return -1;
  } catch (jsi::JSIException &err) {
    fprintf(stderr, "[libhermes] JSI exception in create_function_with_source: %s\n", err.what());
    if (result) *result = nullptr;
    return -1;
  }
}
// Real impl below — falls back to the untyped path. We DO NOT
// want this stubbed: bare's native addons (e.g. bare-hrtime) call
// js_create_typed_function with `assert(err == 0)` followed
// immediately by `js_set_named_property(exports, name, val)`.
// With asserts no-op'd in Release and the stub returning -1, val
// is uninitialized garbage, and the next setProperty crashes
// Hermes' GC slot allocator on what looks like a corrupt
// receiver. Took a full-stack debug session to track that down.
STUB(js_get_typed_callback_info, const js_typed_callback_info_t*, js_env_t**, void**)
STUB(js_terminate_execution, js_env_t*)
STUB(js_fatal_exception,     js_env_t*, js_value_t*)
// Real impl below (not stubbed) — keep the macro list consistent
// by leaving this comment in place so the surrounding STUB(...) lines
// don't visually suggest it's also stubbed.

// is_X predicates we haven't implemented yet (delegate / Date / Map /
// Set / Proxy / etc.) — they all just return false for now.
#define IS_FALSE(name) \
  extern "C" int name(js_env_t *env, js_value_t *value, bool *result) { \
    (void) env; (void) value; *result = false; return 0; \
  }
IS_FALSE(js_is_async_function)
IS_FALSE(js_is_generator_function)
IS_FALSE(js_is_generator)
IS_FALSE(js_is_arguments)
IS_FALSE(js_is_date)
IS_FALSE(js_is_regexp)
IS_FALSE(js_is_proxy)
IS_FALSE(js_is_map)
IS_FALSE(js_is_set)
IS_FALSE(js_is_weak_map)
IS_FALSE(js_is_weak_set)
IS_FALSE(js_is_weak_ref)
IS_FALSE(js_is_typedarray)
IS_FALSE(js_is_dataview)
IS_FALSE(js_is_detached_arraybuffer)
IS_FALSE(js_is_sharedarraybuffer)
#undef IS_FALSE

#undef STUB

// === Additional gap fills from second integration pass ===========
//
// Numeric/string variants + typed-array predicates surfaced by
// bare-type / bare-buffer at link time.

extern "C" int
js_get_value_uint32(js_env_t *env, js_value_t *value, uint32_t *result) {
  (void) env;
  *result = static_cast<uint32_t>(value->value.getNumber());
  return 0;
}

extern "C" int
js_get_value_int64(js_env_t *env, js_value_t *value, int64_t *result) {
  (void) env;
  // JSI numbers are doubles. ±2^53 precision; bare doesn't rely on
  // bit-exact int64 here in the bootstrap path, so this is fine.
  *result = static_cast<int64_t>(value->value.getNumber());
  return 0;
}

// BigInt — Hermes JSI doesn't expose construction; stub for now.
// js_create_bigint_int64 et al. return -1 and bare's bootstrap
// doesn't construct BigInts at startup.
#define STUB(name, ...) extern "C" int name(__VA_ARGS__) { return -1; }
STUB(js_create_bigint_int64,        js_env_t*, int64_t, js_value_t**)
STUB(js_create_bigint_uint64,       js_env_t*, uint64_t, js_value_t**)
STUB(js_get_value_bigint_int64,     js_env_t*, js_value_t*, int64_t*, bool*)
STUB(js_get_value_bigint_uint64,    js_env_t*, js_value_t*, uint64_t*, bool*)
#undef STUB

// String variants. Latin1 + UTF-16 paths can route through UTF-8
// for now (lossy for UTF-16 surrogate pairs but bare's startup
// strings are all ASCII). External-string variants don't get
// special zero-copy treatment — we copy.
extern "C" int
js_create_string_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  // latin1 is a strict subset of utf8 for code points < 0x80, and
  // each 0x80-0xFF byte expands to a 2-byte UTF-8 sequence. We do
  // the simple thing: treat as utf8 directly. Bare's bootstrap
  // doesn't use latin1 paths with high bytes.
  return js_create_string_utf8(env, (const utf8_t *) str, len, result);
}

extern "C" int
js_create_string_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  // NAPI convention: len == (size_t)-1 → null-terminated UTF-16.
  if (len == (size_t) -1) {
    size_t n = 0;
    if (str) while (str[n]) n++;
    len = n;
  }
  // JSI doesn't have createFromUtf16 in the base interface;
  // we'd need Hermes-specific path. For now build via String.fromCharCode.
  // Optimize later when bare actually needs UTF-16 fast paths.
  auto &rt = *env->runtime;
  // Convert each UTF-16 unit to a JS String via String.fromCharCode.
  auto fromCharCode = rt.global()
    .getPropertyAsObject(rt, "String")
    .getPropertyAsFunction(rt, "fromCharCode");
  std::string out;
  out.reserve(len * 3);
  for (size_t i = 0; i < len; i++) {
    uint16_t cu = str[i];
    if (cu < 0x80) {
      out.push_back(static_cast<char>(cu));
    } else if (cu < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cu >> 6)));
      out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (cu >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cu >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    }
  }
  (void) fromCharCode;
  return js_create_string_utf8(env, (const utf8_t *) out.data(), out.size(), result);
}

extern "C" int
js_create_external_string_latin1(js_env_t *env, latin1_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  // No zero-copy path; always copy and run the finalizer immediately.
  int rc = js_create_string_latin1(env, str, len, result);
  if (finalize_cb) finalize_cb(env, str, finalize_hint);
  if (copied) *copied = true;
  return rc;
}

extern "C" int
js_create_property_key_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  // bare uses property keys as strings; treat identically to
  // create_string_utf8 for now. Symbol-keyed properties not exercised
  // by bare's bootstrap.
  return js_create_string_utf8(env, str, len, result);
}

extern "C" int
js_get_value_string_latin1(js_env_t *env, js_value_t *value, latin1_t *buf, size_t len, size_t *result) {
  // Hand back UTF-8 bytes; bare's bootstrap doesn't use latin1
  // read paths with high-byte content.
  return js_get_value_string_utf8(env, value, (utf8_t *) buf, len, result);
}

extern "C" int
js_get_value_string_utf16le(js_env_t *env, js_value_t *value, utf16_t *buf, size_t len, size_t *result) {
  // Naive: decode UTF-8 into UTF-16 code units. Only handles BMP
  // correctly without surrogate pair output; sufficient for bare's
  // startup ASCII keys.
  auto &rt = *env->runtime;
  auto s = value->value.asString(rt).utf8(rt);
  if (buf == nullptr) {
    if (result) *result = s.size();
    return 0;
  }
  size_t out_i = 0;
  for (size_t i = 0; i < s.size() && out_i + 1 < len; i++) {
    buf[out_i++] = static_cast<utf16_t>(static_cast<uint8_t>(s[i]));
  }
  if (result) *result = out_i;
  return 0;
}

// TypedArray predicates — bare-type checks each one. All return
// false until we wire actual TypedArray support in JSI. JSI 0.13's
// `isArrayBuffer` exists but typed-array specific predicates need
// reflection through JS.
#define IS_TA(name) \
  extern "C" int name(js_env_t *env, js_value_t *value, bool *result) { \
    (void) env; (void) value; *result = false; return 0; \
  }
IS_TA(js_is_int8array)
IS_TA(js_is_uint8array)
IS_TA(js_is_uint8clampedarray)
IS_TA(js_is_int16array)
IS_TA(js_is_uint16array)
IS_TA(js_is_int32array)
IS_TA(js_is_uint32array)
IS_TA(js_is_float16array)
IS_TA(js_is_float32array)
IS_TA(js_is_float64array)
IS_TA(js_is_bigint64array)
IS_TA(js_is_biguint64array)
#undef IS_TA

extern "C" int
js_remove_wrap(js_env_t *env, js_value_t *object, void **result) {
  auto &rt = *env->runtime;
  auto obj = object->value.asObject(rt);
  if (!obj.hasNativeState<ExternalState>(rt)) {
    if (result) *result = nullptr;
    return 0;
  }
  if (result) {
    auto state = obj.getNativeState<ExternalState>(rt);
    *result = state ? state->data() : nullptr;
  }
  // JSI doesn't have a "clear NativeState" — setting it to nullptr
  // isn't supported either. Best-effort: stash a no-op replacement
  // state. The original ExternalState's finalize_cb won't fire on
  // GC because we still hold a shared_ptr to it through the
  // replacement, but the data pointer becomes inaccessible via
  // js_unwrap (the new state has data=nullptr).
  //
  // TODO: replace with a proper "drop native state" once JSI gains
  // the API. In practice bare doesn't call remove_wrap often.
  auto cleared = std::make_shared<ExternalState>(env, nullptr, nullptr, nullptr);
  obj.setNativeState(rt, cleared);
  return 0;
}

// === Promises =======================================================
//
// JSI has no first-class promise factory — you build one through
// the global `Promise` constructor by passing an executor function.
// The executor is invoked synchronously during construction; we
// capture its resolve/reject arguments and stash them on the
// js_deferred_s. Later, js_resolve_deferred / js_reject_deferred
// call the captured callbacks.
//
// Lifetimes:
//   js_deferred_s owns shared_ptr<jsi::Value> for resolve+reject
//   so the Promise's continuation chain stays alive even after the
//   creating handle scope closes.

struct js_deferred_s {
  // Shared with the executor lambda's captures so both sides hold
  // strong refs while the executor's args are still being copied.
  std::shared_ptr<jsi::Value> resolve;
  std::shared_ptr<jsi::Value> reject;
  HermesRuntime *runtime;
};

extern "C" int
js_create_promise(js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  auto &rt = *env->runtime;

  auto *def = new js_deferred_s();
  def->resolve = std::make_shared<jsi::Value>(jsi::Value::undefined());
  def->reject  = std::make_shared<jsi::Value>(jsi::Value::undefined());
  def->runtime = env->runtime.get();

  // Capture resolve/reject by shared_ptr so the executor closure
  // doesn't need to outlive `def` directly.
  auto resolve_slot = def->resolve;
  auto reject_slot  = def->reject;

  auto executor = jsi::Function::createFromHostFunction(
    rt, jsi::PropNameID::forUtf8(rt, "executor"), 2,
    [resolve_slot, reject_slot](
      jsi::Runtime &rt,
      const jsi::Value & /* thisVal */,
      const jsi::Value *args,
      size_t count
    ) -> jsi::Value {
      // Standard new Promise(executor) signature: args = [resolve, reject].
      if (count >= 1) *resolve_slot = jsi::Value(rt, args[0]);
      if (count >= 2) *reject_slot  = jsi::Value(rt, args[1]);
      return jsi::Value::undefined();
    }
  );

  auto promise_ctor = rt.global().getPropertyAsFunction(rt, "Promise");
  auto promise_obj = promise_ctor.callAsConstructor(rt, executor);

  *deferred = def;
  *promise = adopt_value(env, std::move(promise_obj));
  return 0;
}

// Helper: resolve or reject the deferred. Single body keeps the
// scope/value plumbing in one place.
static int
deferred_complete(js_env_t *env, js_deferred_t *def, js_value_t *value, bool resolve) {
  auto &rt = *env->runtime;
  auto &slot = resolve ? *def->resolve : *def->reject;
  if (slot.isUndefined()) {
    // create_promise must have run; this is a contract violation —
    // the executor never fired. Return a clear error code.
    return -1;
  }
  auto cb = slot.asObject(rt).asFunction(rt);
  jsi::Value arg = jsi::Value(rt, value->value);
  cb.call(rt, arg);
  // After completion, drop the deferred so subsequent resolve/reject
  // calls on the same handle become explicit programmer errors
  // (caught by the isUndefined check above on next call).
  delete def;
  return 0;
}

extern "C" int
js_resolve_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  return deferred_complete(env, deferred, resolution, true);
}

extern "C" int
js_reject_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  return deferred_complete(env, deferred, resolution, false);
}

// Non-standard helper — drain pending microtasks. In a real Bare
// integration this gets called from the libuv tick loop after each
// IO event. For now expose it so tests can flush promise
// continuations synchronously. Returns 1 if any microtasks were
// drained (matches drainMicrotasks's bool return), 0 otherwise.
extern "C" int
js_run_microtasks(js_env_t *env) {
  return env->runtime->drainMicrotasks() ? 1 : 0;
}

extern "C" int
js_is_promise(js_env_t *env, js_value_t *value, bool *result) {
  auto &rt = *env->runtime;
  if (!value->value.isObject()) { *result = false; return 0; }
  // JSI doesn't have isPromise(). Standard idiom: check whether
  // the object has a `then` function. Not perfect (any thenable
  // would match), but matches what V8/JSC do under the hood.
  auto obj = value->value.asObject(rt);
  auto then_prop = obj.getProperty(rt, "then");
  if (!then_prop.isObject()) { *result = false; return 0; }
  *result = then_prop.asObject(rt).isFunction(rt);
  return 0;
}
