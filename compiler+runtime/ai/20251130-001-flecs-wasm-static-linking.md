# Flecs Static Linking in WASM - Fixes

Date: 2025-11-30

## Summary

Successfully got `my_flecs_static_and_wasm.jank` running in WASM with Flecs ECS library using AOT compilation.

## Issues Fixed

### 1. `-I` Flag Position in emscripten-bundle

**Problem**: The `-I` include path flag was placed after the `run` subcommand:
```bash
jank run --codegen wasm-aot ... -I /path file  # WRONG
```

**Error**: `RequiredError: file is required`

**Solution**: `-I` is a GLOBAL option that must come BEFORE the subcommand:
```bash
jank -I /path run --codegen wasm-aot ... file  # CORRECT
```

**Fix location**: `bin/emscripten-bundle` lines 375-396

### 2. Reader Conditionals vs C++ Preprocessor

**Problem**: Jank reader conditionals (`#?(:jank ... :wasm ...)`) are evaluated at READ-TIME in native jank. When generating WASM AOT code, native jank reads the file with `:jank` features, so the `:jank` branch code gets emitted into the generated C++.

**Example of broken approach**:
```clojure
#?(:jank (cpp/raw "...JIT code...")
   :wasm nil)
```
This STILL emits the JIT code because native jank reads it!

**Solution**: Use C++ preprocessor for platform-specific C++ code:
```clojure
(cpp/raw "
#ifndef JANK_TARGET_WASM
  // JIT-only code
  inline void load_flecs_object() {
    jank::runtime::__rt_ctx->jit_prc.load_object(...);
  }
#else
  // WASM version (no-op or alternative)
  inline void maybe_load_flecs() {}
#endif
")
```

The preprocessor directives are preserved in the generated C++ and evaluated by em++ at compile time.

### 3. Native Header Requires Don't Work in WASM

**Problem**: `(:require ["flecs.h" :as flecs])` in ns declaration throws error in WASM:
```
Error: "Native C++ headers are not supported in WASM"
```

**Solution**: Remove native header requires and use `cpp/raw` blocks directly:
```clojure
;; DON'T do this for WASM:
(ns my-ns
  (:require ["flecs.h" :as flecs]))

;; DO this instead:
(ns my-ns)
(cpp/raw "
#include \"flecs.h\"
// ... inline functions for interop
")
```

## Final Working Pattern

```clojure
(ns my-flecs-static-and-wasm)
;; No native header requires for WASM compatibility

;; Platform-specific code via C++ preprocessor
(cpp/raw "
#ifndef JANK_TARGET_WASM
#include <jank/runtime/context.hpp>
inline void load_flecs_object() {
  jank::runtime::__rt_ctx->jit_prc.load_object(\"/path/to/flecs.o\");
}
inline void maybe_load_flecs() { load_flecs_object(); }
#else
inline void maybe_load_flecs() {}
#endif
")

(cpp/maybe_load_flecs)

;; Common code - works on both platforms
(cpp/raw "
#include \"flecs.h\"
inline jank::runtime::object_ref flecs_create_world() {
  auto* world = new flecs::world();
  return jank::runtime::make_box<jank::runtime::obj::opaque_box>(
    static_cast<void*>(world), \"flecs::world\");
}
// ... more interop functions
")
```

## Commands

Build and run:
```bash
./bin/emscripten-bundle --skip-build --run \
  -I /path/to/flecs/distr \
  --lib /path/to/flecs_wasm.o \
  /path/to/my_flecs_static_and_wasm.jank
```

## Key Takeaways

1. **Global CLI options before subcommand**: `-I`, `--module-path`, etc. must come before `run`, `repl`, etc.
2. **Reader conditionals are read-time**: Use C++ `#ifdef` for AOT platform detection
3. **Native header requires don't work in WASM**: Use `cpp/raw` blocks with explicit `#include`
4. **C++ preprocessor macros available**: `JANK_TARGET_WASM=1` and `JANK_TARGET_EMSCRIPTEN` are defined for WASM builds

### 4. WASM Module Exit Code Handling

**Problem**: When WASM `main()` returns a non-zero exit code (e.g., due to an exception), the Node.js runner still reported "Module executed successfully" because Emscripten doesn't throw a JavaScript exception for non-zero exit codes.

**Solution**: Modified the Node.js runner in `bin/emscripten-bundle` to check `Module.EXITSTATUS` after the module runs:
```javascript
.then((Module) => {
  const exitStatus = Module.EXITSTATUS !== undefined ? Module.EXITSTATUS : 0;
  if (exitStatus !== 0) {
    console.error('[jank-runner] Module exited with code ' + exitStatus);
    process.exit(exitStatus);
  }
  console.log('[jank-runner] Module executed successfully');
})
```

This ensures that WASM runtime errors cause the build to fail properly.

### 5. Identical File Copy Error

**Problem**: When the jank source file is already in the output directory (build-wasm), the `cp` command to copy it for debugging fails with exit code 1:
```
cp: file1 and file2 are identical (not copied).
```

**Solution**: Check if source and destination are the same file before copying:
```bash
dest_file="${output_dir}/$(basename "${jank_source}")"
if [[ ! "${jank_source}" -ef "${dest_file}" ]]; then
  cp "${jank_source}" "${output_dir}/"
fi
```

**Fix location**: `bin/emscripten-bundle` lines 1196-1201

## Status

✅ **WORKING** - Flecs WASM integration is fully functional. The module:
- Creates Flecs world in WASM
- Creates entities
- Progresses the world simulation
- All with exit code 0
