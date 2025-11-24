# Compiling jank to WebAssembly - Current Status

## Summary

The jank runtime has been successfully built for WebAssembly, but **compiling arbitrary jank source code (like `hello.jank`) to WASM is not yet fully implemented**. Here's what works and what doesn't:

## ✅ What Works

1. **WASM Runtime**: The minimal jank runtime builds successfully with emscripten
   - Location: `../build-wasm/libjank.a`
   - Size: ~4.6 KB
   - Includes: panic handler, assert handler, JIT stub

2. **Manual C++ Integration**: You can write C++ code that uses the jank runtime
   - See `jank_demo.cpp` for an example
   - Compile with: `em++ jank_demo.cpp ../build-wasm/libjank.a -o jank_demo.js`

## ❌ What Doesn't Work Yet

### Problem: No Path from `.jank` Source to WASM

The command:
```bash
./build/jank compile --codegen cpp --runtime static -o hello my-ns
```

Does NOT generate standalone C++ files that can be compiled to WASM because:

1. **C++ Codegen is JIT-Only**: The `--codegen cpp` option only works during JIT compilation (dynamic loading), not during AOT compilation
2. **AOT Uses LLVM IR**: The `compile` command with static runtime uses LLVM IR directly, not C++
3. **Module Interdependencies**: Even if we could extract C++ for one module, handling dependencies and the full runtime is complex

### What's Missing

To compile `hello.jank` to WASM, we would need:

1. **C++ Code Generation for Modules**
   - Extract generated C++ for each jank module
   - Currently only happens during JIT evaluation

2. **Dependency Resolution**
   - Generate C++ for all dependencies (like clojure.core)
   - Link them properly with the WASM runtime

3. **Runtime Integration**
   - The eval/compile functions need to work in WASM
   - Or we need full AOT compilation that doesn't require runtime compilation

## 🔧 Current Workarounds

### Option 1: Manual C++ Wrapper (Recommended for Now)

Write C++ code that manually constructs what your jank code does:

```cpp
#include <emscripten.h>

extern "C" {

EMSCRIPTEN_KEEPALIVE
void my_jank_function() {
    // Manually call jank runtime functions
    // This is what jank_demo.cpp does
    emscripten_run_script("console.log('Hello from manual jank!')");
}

int main() {
    my_jank_function();
    return 0;
}
}
```

Compile:
```bash
em++ -O2 my_wrapper.cpp ../build-wasm/libjank.a -o output.js \
  -s EXPORTED_FUNCTIONS='["_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'
```

### Option 2: Extract C++ During Compilation (Experimental)

I've added a feature to save generated C++ code:

```bash
# This modification was added to context.cpp
# Set JANK_SAVE_CPP=1 to save generated C++ files during module compilation
JANK_SAVE_CPP=1 ./build/jank compile-module --codegen cpp my-ns
```

However, this currently **does not work** because:
- `compile-module` doesn't exist as a command
- The `compile` command doesn't use cpp codegen for modules
- Generated C++ would be incomplete without proper linking

## 📋 Steps to Build WASM Runtime

1. Build the WASM version of jank runtime:
```bash
cd compiler+runtime
./bin/emscripten-bundle
```

2. This creates `build-wasm/libjank.a`

3. Write your own C++ wrapper (see above)

4. Compile with emscripten:
```bash
cd wasm-examples
em++ -O2 your_code.cpp ../build-wasm/libjank.a -o your_code.js \
  -s EXPORTED_FUNCTIONS='["_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'
```

5. Serve and test:
```bash
python3 -m http.server 8080
# Open http://localhost:8080/your_code.html
```

## 🎯 What Would Be Needed for Full Support

To properly compile `hello.jank` (or any `.jank` file) to WASM:

### Short-term (Hacky but Functional)

1. **Add CPP Codegen to AOT Path**
   - Modify `aot/processor.cpp` to optionally generate C++ instead of LLVM IR
   - Save generated C++ files to disk
   - Provide a way to compile them with em++

2. **Module Registry**
   - Pre-compile all clojure.core modules to C++
   - Create a static registry of available modules
   - No dynamic loading needed

### Long-term (Proper Solution)

1. **Full AOT Compilation**
   - Compile entire program (including all dependencies) to LLVM IR
   - Use LLVM's WASM backend to generate WASM directly
   - No C++ intermediate step needed

2. **WASM-aware Runtime**
   - Port the minimal required runtime to WASM
   - Remove JIT dependencies entirely for WASM builds
   - Static linking of all required modules

## 📚 See Also

- `../docs/wasm-minimal-example.md` - Overview of WASM compilation plan
- `../bin/emscripten-bundle` - Script to build WASM runtime
- `jank_demo.cpp` - Example of manual C++ wrapper
- `hello_wasm.cpp` - Simplest WASM example (no jank runtime)

## 🐛 Current Build Issues

The `JANK_SAVE_CPP=1` feature was added but is not yet working because:
1. The code path for C++ codegen during `compile` is not executed for modules
2. Would need significant refactoring to extract C++ for modules during AOT compilation

## ✨ Summary

**For now**: Use manual C++ wrappers with the WASM runtime  
**Future**: Full jank→WASM compilation pipeline needs to be built
