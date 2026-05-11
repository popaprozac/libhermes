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

**Phase 1: in progress.** Build pipeline works end-to-end and the
first smoke test passes:

```
$ bun install
$ cmake -B build
$ cmake --build build --target eval-script
$ ./build/test/eval-script
[eval-script] result: 'hello, hermes' (len=13)
[eval-script] OK
```

That exercises platform / env creation, handle scopes, string
creation + UTF-8 readback, and `js_run_script` through Hermes'
JSI. Roughly 13 of 19 Phase 1 js_* functions are implemented; the
remaining gap is `js_create_function` / `js_call_function` (the
JSI HostFunction binding), which is the next thing to land before
hooking into Bare proper.

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
