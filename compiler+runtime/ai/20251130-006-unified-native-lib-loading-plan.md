# Unified Native Library Loading for jank (Native + WASM)

Date: 2025-11-30

## Problem Statement

Currently, using native C/C++ libraries in jank requires different approaches for different targets:

| Target | Current Approach | Pain Point |
|--------|-----------------|------------|
| Native jank (JIT) | `--obj flecs.o` or `-l libflecs.dylib` | Must pre-compile for host platform |
| Native jank (AOT for WASM) | `--native-obj flecs.o` in emscripten-bundle | Separate option, easy to forget |
| WASM runtime | `--lib flecs_wasm.o` in emscripten-bundle | Must pre-compile for WASM |

**Goal**: Single, simple interface that "just works" for all targets.

---

## Option 1: Auto-Detect Native from WASM Library

**Approach**: When `--lib foo_wasm.o` is specified, automatically search for native equivalent.

**Search order**:
1. `foo.o` (strip `_wasm` suffix)
2. `libfoo.dylib` / `libfoo.so` (dynamic library)
3. `libfoo.a` (static archive - would need extraction)
4. `foo.c` (compile on-the-fly)

**Pros**:
- No new options needed
- Convention-based, predictable
- Backwards compatible

**Cons**:
- Magic behavior might be confusing
- Naming conventions might not always match
- Still requires pre-compiled native library

**Implementation**: ~50 lines in emscripten-bundle

---

## Option 2: Compile from Source On-Demand

**Approach**: Accept `.c`/`.cpp` source files and compile them for both targets.

```bash
# Single source, works for both native and WASM
jank --lib-src vendor/flecs/flecs.c run-main my-app

./bin/emscripten-bundle --lib-src vendor/flecs/flecs.c my_app.jank
```

**How it works**:
1. For native jank: Compile with host clang, load into JIT
2. For WASM: Compile with emcc, link into bundle

**Pros**:
- Single source of truth
- No pre-compilation needed
- Works universally

**Cons**:
- Slow for large libraries (Flecs is 2.7MB of C)
- Need to cache compiled objects
- Complex compiler flag handling

**Implementation**: Moderate complexity, needs build caching

---

## Option 3: Header-Only Mode for Native AOT

**Approach**: During AOT compilation, don't actually execute native code - just generate C++ that will call it at runtime.

**How it works**:
1. For `(:require ["flecs.h" :as flecs])`, only register metadata during AOT
2. Don't try to resolve/call flecs symbols during AOT
3. Symbols only need to exist at WASM runtime

**The challenge**: jank currently *runs* top-level code during AOT:
```clojure
;; This calls flecs during AOT compilation!
(def world (flecs/world))
```

**Solution**: Wrap in a reader conditional or defer to runtime:
```clojure
#?(:wasm (def world (flecs/world)))  ; Only at WASM runtime
```

**Pros**:
- No native library needed at all during AOT
- Simplest user experience
- Fast compilation

**Cons**:
- Changes semantics of top-level evaluation
- May require code changes in user projects
- Some compile-time checks lost

**Implementation**: Would require changes to how `--codegen wasm-aot` handles evaluation

---

## Option 4: Unified `--lib` with Smart Resolution

**Approach**: Make `--lib` accept a "library spec" that works across all targets.

```bash
# Library spec format: name:native=path,wasm=path
jank --lib "flecs:native=./flecs.o,wasm=./flecs_wasm.o" run-main my-app

# Or simpler, just a directory:
jank --lib ./vendor/flecs/distr run-main my-app
# Looks for: flecs.o, flecs_wasm.o, libflecs.dylib, etc.
```

**Pros**:
- Explicit and clear
- Single option for everything
- Flexible

**Cons**:
- More complex syntax
- Still requires both libraries to exist

---

## Option 5: Dynamic Library for Native, Object for WASM

**Approach**: Use dynamic libraries for native jank (already supported via `-l`), object files only for WASM.

```bash
# Native: use dylib (loads via dlopen)
jank -l ./vendor/flecs/libflecs.dylib run-main my-app

# WASM: use object file
./bin/emscripten-bundle --lib ./vendor/flecs/flecs_wasm.o my_app.jank
```

**emscripten-bundle enhancement**: Auto-detect dylib from `--lib` path:
- If `--lib foo_wasm.o`, look for `libfoo.dylib` and pass as `-l` to native jank

**Pros**:
- Uses existing mechanisms
- No new jank features needed
- dylibs are more portable

**Cons**:
- Still two different files
- dylib creation adds a step

---

## Recommendation

**Short-term (easiest)**: Implement **Option 1** or **Option 5**
- Auto-detect native library from WASM library path
- Minimal changes, immediate value
- Keep `--native-obj` as explicit override

**Medium-term**: Implement **Option 2** (compile from source)
- Best user experience for simple cases
- Add build caching for performance
- Libraries like Flecs distribute as single .c/.h

**Long-term**: Consider **Option 3** (defer evaluation)
- Cleanest separation of compile-time vs runtime
- Requires thinking through semantics

---

## Proposed Short-Term Implementation

Modify `bin/emscripten-bundle` to auto-detect native libraries:

```bash
# Given: --lib /path/to/flecs_wasm.o
# Search for native library in same directory:

find_native_lib() {
  local wasm_lib="$1"
  local dir=$(dirname "$wasm_lib")
  local base=$(basename "$wasm_lib" .o)

  # Strip _wasm suffix if present
  local native_base="${base%_wasm}"

  # Try in order: .o, .dylib, .so, .a
  for ext in .o .dylib .so .a; do
    local candidate="${dir}/${native_base}${ext}"
    [[ -f "$candidate" ]] && echo "$candidate" && return

    # Try lib prefix
    candidate="${dir}/lib${native_base}${ext}"
    [[ -f "$candidate" ]] && echo "$candidate" && return
  done
}
```

Then in AOT compilation section:
```bash
# Auto-detect native libraries from --lib entries
for wasm_lib in "${extra_libs[@]}"; do
  native_lib=$(find_native_lib "$wasm_lib")
  if [[ -n "$native_lib" ]]; then
    case "$native_lib" in
      *.dylib|*.so) jank_cmd+=(-l "$native_lib") ;;
      *.o|*.a)      jank_cmd+=(--obj "$native_lib") ;;
    esac
  fi
done
```

This way, users just need:
```bash
./bin/emscripten-bundle --lib ./vendor/flecs/flecs_wasm.o my_app.jank
# Automatically finds and uses ./vendor/flecs/flecs.o for native AOT
```

---

## Testing Checklist

- [ ] `--lib foo_wasm.o` finds `foo.o` in same dir
- [ ] `--lib foo_wasm.o` finds `libfoo.dylib` if no `.o`
- [ ] `--native-obj` still works as explicit override
- [ ] Warning if no native library found
- [ ] Works with multiple `--lib` entries
