# Step 1 Complete: MAIN_MODULE Support Added

**Date:** Nov 27, 2025
**Status:** ✅ Complete

## Summary

Successfully added hot-reload support to jank's emscripten-bundle script. The WASM bundle can now be built with dynamic linking enabled, allowing runtime loading of function patches via `dlopen()`.

## Changes Made

### File: `compiler+runtime/bin/emscripten-bundle`

**Lines 1165-1189:** Added `HOT_RELOAD=1` environment variable mode

```bash
# Hot-reload support for REPL-like development
# Set HOT_RELOAD=1 to enable MAIN_MODULE mode for runtime function patching
if [[ "${HOT_RELOAD:-}" == "1" ]]; then
  echo "[emscripten-bundle] HOT-RELOAD MODE: Enabling dynamic linking (-sMAIN_MODULE=2)"
  echo "[emscripten-bundle] This allows loading function patches at runtime via dlopen"
  echo "[emscripten-bundle] WARNING: Bundle size will increase (~100-150MB)"
  em_link_cmd+=(
    -sMAIN_MODULE=2                     # Optimized dynamic linking
    -sALLOW_TABLE_GROWTH=1              # Allow adding functions to indirect call table
    -sEXPORTED_RUNTIME_METHODS=FS,stringToNewUTF8,ccall,cwrap,UTF8ToString
    -sFILESYSTEM=1                      # Required for dlopen to work
    -Wl,--allow-multiple-definition     # Handle bdwgc symbol conflicts
    -sEXPORT_ALL=1                      # Export all symbols for dlsym
  )
fi
```

**Lines 709-712:** Added `-fPIC` for hot-reload mode

```bash
# Add -fPIC for hot-reload mode (required for MAIN_MODULE dynamic linking)
if [[ "${HOT_RELOAD:-}" == "1" ]]; then
  jank_compile_flags+=(-fPIC)
fi
```

## Usage

### Build jank WASM with hot-reload support:

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Option 1: Build runtime only
HOT_RELOAD=1 ./bin/emscripten-bundle

# Option 2: Build with user code
HOT_RELOAD=1 ./bin/emscripten-bundle your_app.jank

# Option 3: Build and run
HOT_RELOAD=1 ./bin/emscripten-bundle --run your_app.jank
```

### Compile function patches (side modules):

```bash
# Example: Compile a patched function
source ~/emsdk/emsdk_env.sh
emcc patched_function.cpp -o patch.wasm -sSIDE_MODULE=1 -O2 -fPIC
```

## Technical Details

### Emscripten Flags Explained

| Flag | Purpose |
|------|---------|
| `-sMAIN_MODULE=2` | Enable dynamic linking (MODE 2 = optimized, smaller than MODE 1) |
| `-sALLOW_TABLE_GROWTH=1` | Allow indirect function table to grow at runtime |
| `-sFILESYSTEM=1` | Enable Emscripten virtual FS (required for dlopen paths) |
| `-sEXPORT_ALL=1` | Export all symbols so dlsym can find them |
| `-fPIC` | Position-independent code (required for dynamic linking) |
| `-Wl,--allow-multiple-definition` | Handle symbol conflicts between bdwgc and libc |

### MAIN_MODULE vs SIDE_MODULE

- **MAIN_MODULE** = The main WASM bundle (jank runtime + user code)
  - Built with `-sMAIN_MODULE=1` or `-sMAIN_MODULE=2`
  - Can call `dlopen()` to load SIDE_MODULEs
  - Larger size (~100-150MB) due to dynamic linking overhead

- **SIDE_MODULE** = Function patches loaded at runtime
  - Built with `-sSIDE_MODULE=1`
  - Tiny size (~100-200 bytes per function)
  - Loaded via `dlopen()` in MAIN_MODULE

### Size Impact

| Build Mode | WASM Size | Description |
|------------|-----------|-------------|
| Normal | ~60MB | Static linking, no hot-reload |
| HOT_RELOAD=1 | ~100-150MB | Dynamic linking enabled |
| Side module | ~100-200 bytes | Single function patch |

The size increase is acceptable for development mode. Production builds can still use normal mode.

## Next Steps

**Step 2:** Implement var registry in jank runtime
**Step 3:** Add WebSocket bridge for nREPL communication
**Step 4:** Implement server-side patch compilation

See `INTEGRATION.md` for detailed implementation plans.

## Testing

To test that MAIN_MODULE support works:

1. Build with hot-reload:
   ```bash
   HOT_RELOAD=1 ./bin/emscripten-bundle
   ```

2. Check the output for:
   ```
   [emscripten-bundle] HOT-RELOAD MODE: Enabling dynamic linking (-sMAIN_MODULE=2)
   ```

3. Verify the WASM includes dlopen support:
   ```bash
   # Check for dlopen symbols in the generated WASM
   wasm-objdump -x build-wasm/jank.wasm | grep dlopen
   ```

4. Test loading a side module (proof of concept exists in `/hot-reload-test/`)

## References

- Proof of concept: `/Users/pfeodrippe/dev/jank/wasm-clang-interpreter-test/hot-reload-test/`
- Emscripten dynamic linking: https://emscripten.org/docs/compiling/Dynamic-Linking.html
- Hot-reload architecture: `hot-reload-test/README.md`

---

*Implementation by Claude Code, Nov 27, 2025*
