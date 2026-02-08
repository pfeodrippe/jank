# Flecs WASM Integration with jank

## Problem
Integrating external C/C++ libraries (like Flecs ECS) with jank's WASM/Emscripten build system requires special handling because:
1. Native JIT loading doesn't work in WASM
2. Object files must be compiled specifically for WASM (32-bit) vs native (64-bit)
3. The emscripten-bundle script needed flags for include paths and external libraries

## Solution

### 1. Added `-I` and `--lib` flags to emscripten-bundle

In `bin/emscripten-bundle`:
```bash
extra_include_paths=()
extra_libs=()

# Argument parsing
-I)
  extra_include_paths+=("$2")
  shift 2
  ;;
--lib)
  extra_libs+=("$2")
  shift 2
  ;;

# Adding include paths (with empty array guard)
if [[ ${#extra_include_paths[@]} -gt 0 ]]; then
  for inc_path in "${extra_include_paths[@]}"; do
    jank_include_flags+=(-I"${inc_path}")
  done
fi

# Adding extra libs (with empty array guard)
if [[ ${#extra_libs[@]} -gt 0 ]]; then
  for lib_path in "${extra_libs[@]}"; do
    em_link_cmd+=("${lib_path}")
  done
fi
```

**Important**: Empty bash arrays cause "unbound variable" errors with `set -u`. Always guard with `if [[ ${#array[@]} -gt 0 ]]` before iterating.

### 2. Compile libraries for WASM separately

Native `.o` or `.bc` files won't work - they're 64-bit. Compile fresh for WASM:

```bash
# Compile Flecs for WASM
emcc -c flecs.c -o flecs_wasm.o -O2

# Compile C++ wrapper for WASM
em++ -c flecs_jank_wrapper.cpp -o flecs_jank_wrapper.o -I. -O2 -std=c++17

# Also compile native wrapper for JIT during AOT
clang++ -c flecs_jank_wrapper.cpp -o flecs_jank_wrapper_native.o -I. -O2 -std=c++17
```

### 3. Use wrapper functions with C linkage

Create a wrapper with `extern "C"` to avoid C++ name mangling:

```cpp
// flecs_jank_wrapper.cpp
#include "flecs.hpp"
#include <cstdint>

extern "C" {
void* jank_flecs_create_world() {
    flecs::world* world = new flecs::world();
    return static_cast<void*>(world);
}

void jank_flecs_destroy_world(void* world_ptr) {
    delete static_cast<flecs::world*>(world_ptr);
}

uint64_t jank_flecs_create_entity(void* world_ptr) {
    flecs::world* world = static_cast<flecs::world*>(world_ptr);
    return world->entity().id();
}
}
```

### 4. Conditional loading in jank source

Use `#ifndef JANK_TARGET_WASM` to load native libs only for JIT:

```clojure
(cpp/raw "
#include <jank/runtime/context.hpp>

#ifndef JANK_TARGET_WASM
inline void load_flecs_native_libs() {
  jank::runtime::__rt_ctx->jit_prc.load_object(\"/path/to/flecs.o\");
  jank::runtime::__rt_ctx->jit_prc.load_object(\"/path/to/flecs_jank_wrapper_native.o\");
}
#else
inline void load_flecs_native_libs() {}
#endif
")
(cpp/load_flecs_native_libs)
```

### 5. Build command

```bash
./bin/emscripten-bundle --skip-build --run --force-regenerate \
  --lib /path/to/flecs_wasm.o \
  --lib /path/to/flecs_jank_wrapper.o \
  /path/to/my_flecs_wasm.jank
```

## Key Learnings

1. **Namespace demunging for exports**: File names use underscores (`my_flecs_wasm.jank`) but namespaces use hyphens (`my-flecs-wasm`). Fixed in `main.cpp` to convert underscores to hyphens before namespace lookup for `^:export` processing.

2. **64-bit vs 32-bit**: WASM is 32-bit; native is typically 64-bit. Function signatures with pointers will mismatch if you use wrong object files.

3. **Wrapper pattern**: Create C wrapper functions that jank can declare with `extern "C"` and call. The wrapper handles C++ class instantiation.

4. **opaque_box for pointers**: Use `jank::runtime::obj::opaque_box` to wrap void* pointers and pass them through jank.

5. **Avoid load-time execution**: Some libraries (like Flecs) may have issues when called during AOT compilation. Define functions but don't call them at load time - let the user invoke from browser.

6. **^:export requirements**: Exported functions must take exactly 1 numeric parameter and return a number (the generated wrapper uses `double` for JS compatibility).

7. **Stack size for complex libraries**: The default 64KB WASM stack is too small for libraries like Flecs. Added `-sSTACK_SIZE=2097152` (2MB) to emscripten-bundle.

## Files Created

- `flecs_jank_wrapper.cpp` - C wrapper for Flecs C++ API
- `flecs_wasm.o` - Flecs compiled for WASM
- `flecs_jank_wrapper.o` - Wrapper compiled for WASM
- `flecs_jank_wrapper_native.o` - Wrapper compiled for native JIT
- `my_flecs_wasm.jank` - jank source demonstrating integration
