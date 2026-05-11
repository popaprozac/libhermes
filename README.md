# libhermes

[Bare](https://github.com/holepunchto/bare) engine binding for
[Meta's Hermes](https://github.com/facebook/hermes) JavaScript
engine. ABI-compatible with
[holepunchto/libjs](https://github.com/holepunchto/libjs) —
exposes the same `js.h` interface every other Bare engine binding
(libjs / libjsc / libqjs / libmqjs) implements, so Bare runtimes
and `bare-*` native modules link against it identically.

## Why Hermes?

Hermes was designed jitless from day 1 for React Native's iOS
constraints. That's the same constraint set non-browser apps hit
on the App Store today — no `mmap(PROT_EXEC)`, no V8 JIT — so
Hermes is uniquely well-suited as a *first-class* mobile JS engine
for Bare-runtime apps.

Concrete wins:

- **AOT bytecode** via `hermesc` — ship pre-compiled `.hbc` files
  and skip parse entirely on worker spawn.
- **~1.5–2 MB binary** — comparable to QuickJS at much better
  sustained perf via Hermes' tiered interpreter.
- **Battle-tested at scale** — React Native ships Hermes to
  billions of devices.
- **No JIT entitlement headaches** — App Store-clean by design.

## Status

**Phase 1: complete + most of Phase 2 surface.** Seven end-to-end
tests pass against Hermes:

```
$ bun install
$ cmake -B build && cmake --build build -j4
$ for t in eval-script host-function global-host-binding \
           data-roundtrip exception-flow arraybuffer-roundtrip \
           promise-flow; do
    ./build/test/$t || break
  done
```

Functional coverage:

| Area | Status |
|---|---|
| platform / env lifecycle | ✅ |
| handle scopes | ✅ |
| primitives (undef/null/bool/i32/u32/i64/f64) | ✅ |
| type predicates (typeof + 12 is_X) | ✅ |
| strings (UTF-8 create + readback) | ✅ |
| objects + arrays (create / get / set / length / delete) | ✅ |
| functions (host fn + call + callback_info) | ✅ |
| run_script + eval result capture | ✅ |
| exception flow (host→JS, JS→host) | ✅ |
| persistent references (scope-crossing) | ✅ |
| externals (NativeState-backed void*) | ✅ |
| ArrayBuffer (owned + external, info/is) | ✅ |
| promises (create + resolve/reject + microtask drain) | ✅ |
| BigInt | ✗ |
| Symbol, Date, RegExp details | ✗ |
| Module loading (Module/SyntheticModule) | ✗ |
| TypedArray / DataView | ✗ |
| Wrap / unwrap (class-style C↔JS binding) | ✗ |
| Property descriptors (define_properties) | ✗ |
| Threadsafe functions (libuv ↔ JS) | ✗ |

Planned phases:

1. **Binding** — implement the js.h surface against Hermes' JSI /
   `hermes::vm::Runtime`. Target ~2-3K LOC by analogy with libqjs.
   Foundation in place; remaining work is the HostFunction wrapper
   and the rest of the object/array surface.
2. **AOT bytecode** — Vite/CLI step that runs `hermesc` over
   bundled worker .mjs to emit `.hbc`. Worker spawn parse cost
   drops to near zero.
3. **iOS default** — once stable, swap the default Bare engine on
   iOS from bare-jsc to bare-hermes in
   [@zappdev/cli](https://github.com/popaprozac/zapp).

## Local dev

```
bun install
cmake -B build && cmake --build build --target js_static
cmake --build build --target eval-script
./build/test/eval-script
```

First cmake configure pulls Hermes (~6 MB src) via cmake-fetch +
takes ~45s. Subsequent configures are cached.

## License

Apache-2.0 (binding); Hermes itself is MIT.
