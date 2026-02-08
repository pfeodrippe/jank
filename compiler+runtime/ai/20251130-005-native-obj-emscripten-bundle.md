# --native-obj Option for emscripten-bundle

Date: 2025-11-30

## Summary

Added `--native-obj` option to `emscripten-bundle` script for loading native object files during AOT compilation.

## Problem

When compiling jank code that uses native C/C++ libraries (like Flecs) to WASM:
- The `--lib` option specifies WASM object files for linking
- But native jank's JIT needs the *native* object file during AOT compilation to resolve symbols
- Without this, AOT compilation fails with "Symbols not found" errors

## Solution

Added `--native-obj` option that passes `--obj` to native jank during AOT compilation:
- `--lib` = WASM object files for em++ linking
- `--native-obj` = Native object files for JIT during AOT codegen

### Files Modified

**`bin/emscripten-bundle`**:
1. Added `native_obj_files=()` array initialization
2. Added `--native-obj` to usage text
3. Added argument parsing for `--native-obj`
4. Added code to pass `--obj` flags to jank during AOT compilation

## Usage

```bash
./bin/emscripten-bundle --skip-build --run \
    --native-obj /path/to/library.o \       # For native JIT during AOT
    --lib /path/to/library_wasm.o \         # For WASM linking
    -I /path/to/headers \
    my_source.jank
```

## Example with Flecs

```bash
./bin/emscripten-bundle --skip-build --run \
    --native-obj /Users/pfeodrippe/dev/something/vendor/flecs/distr/flecs.o \
    --lib /Users/pfeodrippe/dev/something/vendor/flecs/distr/flecs_wasm.o \
    -I /Users/pfeodrippe/dev/something/vendor/flecs/distr \
    /Users/pfeodrippe/dev/something/src/my_flecs_static_and_wasm.jank
```

## How It Works

1. User provides both `--native-obj` (for native jank) and `--lib` (for WASM linking)
2. During AOT compilation, `emscripten-bundle` passes `--obj` to native jank
3. Native jank loads the object file into JIT, providing symbols for code generation
4. The generated C++ is then compiled with em++ and linked with the WASM object file

## Notes

- The native and WASM object files can be different compilations of the same library
- Native: compiled for host platform (x86_64/arm64)
- WASM: compiled with emscripten for WebAssembly
- Object files are resolved to absolute paths before being passed to jank
