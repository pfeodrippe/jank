# CLI Option for Loading Object Files

Date: 2025-11-30

## Summary

Added `--obj` CLI option to load `.o` (object) files directly into the JIT, similar to how `--lib` works in WASM emscripten-bundle.

## Problem

Previously, loading `.o` files in native jank required using `cpp/raw` to call `jit_prc.load_object()` with a hardcoded path:

```clojure
(cpp/raw "
#include <jank/runtime/context.hpp>
inline void load_flecs_object() {
  jank::runtime::__rt_ctx->jit_prc.load_object(\"/path/to/flecs.o\");
}
")
(cpp/load_flecs_object)
```

The `-l` option only worked with dynamic libraries (.so/.dylib) via `dlopen()`, not object files.

## Solution

Added `--obj` CLI option that loads object files into the JIT at startup.

### Files Modified

1. **`include/cpp/jank/util/cli.hpp`** - Added `object_files` field to options struct:
   ```cpp
   native_vector<jtl::immutable_string> object_files;
   ```

2. **`src/cpp/jank/util/cli.cpp`** - Added CLI option:
   ```cpp
   cli.add_option("--obj",
                  opts.object_files,
                  "Absolute or relative path to object files (.o) to load into JIT. "
                  "Can be specified multiple times.");
   ```

3. **`src/cpp/jank/jit/processor.cpp`** - Load object files during JIT initialization:
   ```cpp
   /* Load object files from --obj CLI option. */
   for(auto const &obj_path : util::cli::opts.object_files)
   {
     load_object(obj_path);
   }
   ```

## Usage

```bash
jank -I./vendor/flecs/distr --module-path src --obj ./vendor/flecs/distr/flecs.o run-main my-module
```

Multiple object files can be specified:
```bash
jank --obj file1.o --obj file2.o run-main my-module
```

## Testing

```bash
cd /Users/pfeodrippe/dev/something
jank -I./vendor/flecs/distr --module-path src --obj ./vendor/flecs/distr/flecs.o run-main my-flecs-static-and-wasm
```

Output:
```
Starting Flecs demo... [true true]
Flecs world created (via static object file)!
World: #object [opaque_box 0x106f8c640]
Entity created: 578
World progressed!
Done!
```

## Notes

- This complements the existing `-l` option which is for dynamic libraries
- Object files are loaded early during JIT initialization, before any jank code runs
- Works with any `.o` file compiled for the host platform
