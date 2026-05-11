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

  // Set by js_on_uncaught_exception; called from JSI's
  // ::setThrowFromHostFunction hook (Hermes routes thrown
  // exceptions through that — TODO confirm exact API on first use).
  js_uncaught_exception_cb on_uncaught = nullptr;
  void *on_uncaught_data = nullptr;
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
  env->runtime = ::facebook::hermes::makeHermesRuntime();
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
      (void) rt;
      js_callback_info_s info{env, &thisVal, args, count, data};
      js_value_t *ret = cb(env, &info);
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

  // Resolve the callee — must be a jsi::Function (an Object that
  // passes Function::isFunction). asObject() throws JSError
  // otherwise; we let that propagate for now and capture it via
  // the uncaught-exception path once that's wired.
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
  // `call(rt, const Value*, size_t)` / `callWithThis(rt, Object, const Value*, size_t)`
  // overloads. Without the const, Hermes' variadic template
  // overload wins resolution and tries to convert (Value*, size_t)
  // into JS args directly.
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
  auto out = rt.evaluateJavaScript(buffer, file ? file : "<script>");
  *result = adopt_value(env, std::move(out));
  return 0;
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
