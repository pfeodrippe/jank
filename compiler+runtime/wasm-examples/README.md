# jank WASM Examples

This directory contains example jank programs for testing WebAssembly compilation.

## Running a .jank file with WASM AOT codegen

The `--codegen wasm_aot` flag generates C++ code that can be compiled by Emscripten for WebAssembly.

### Quick Start

```bash
# Build a jank file for WASM and run with Node.js
cd compiler+runtime
./bin/emscripten-bundle --skip-build wasm-examples/minimal.jank
cd build-wasm
node -e "const m = require('./minimal.js'); m()"
```

### Example Output

```
[jank-wasm] jank WebAssembly Runtime (AOT)
[jank-wasm] Module: minimal

[jank-wasm] Calling jank_init...
[jank-wasm] AOT mode: using pre-compiled code
[jank-wasm] Loading clojure.core-native...
[jank-wasm] Core native loaded!

[jank-wasm] Executing pre-compiled module: minimal
[jank-wasm] Result: 0x175d68

[jank-wasm] Done (AOT mode)!
```

### Basic Usage

```bash
# From the compiler+runtime directory
./build/jank run --codegen wasm_aot --save-cpp --save-cpp-path output.cpp your_file.jank
```

### Options

| Flag | Description |
|------|-------------|
| `--codegen wasm_aot` | Use WASM AOT code generation |
| `--save-cpp` | Save the generated C++ code |
| `--save-cpp-path <path>` | Path where to save the C++ file |

### Example

```bash
# Run eita.jank and generate C++ for WASM
cd wasm-examples
../build/jank run --codegen wasm_aot --save-cpp --save-cpp-path output.cpp eita.jank
```

This will:
1. Execute the jank code on the host (showing output)
2. Generate C++ code suitable for Emscripten compilation
3. Save the C++ to `output.cpp`

### Example Files

| File | Description |
|------|-------------|
| `minimal.jank` | **Working** - Returns 42, no clojure.core deps |
| `simple.jank` | Simple expressions |
| `loop_test.jank` | Tests loop/recur |
| `native_call.jank` | Tests native function calls |
| `eita.jank` | Full test with clojure.set (requires clojure.core) |

### Current Limitations

- **clojure.core functions**: Code using `println`, `map`, etc. requires the full clojure.core to be AOT compiled and loaded
- **Complex data structures**: Some collection operations may not work due to missing type implementations in WASM build

### Generated C++ Structure

The generated C++ includes:
- Namespace definitions for each module
- Struct definitions for each function  
- `extern "C"` registration functions:
  - `jank_load_<module>()` - Standard module load function
  - `jank_wasm_init_<module>()` - WASM-specific init returning `object_ref`

### Building for WASM with Emscripten

Once you have the generated C++, you can compile it with the jank runtime using `emscripten-bundle`:

```bash
# From compiler+runtime directory
./bin/emscripten-bundle wasm-examples/eita.jank
```

This generates:
- `build-wasm/eita.js` - JavaScript loader module
- `build-wasm/eita.wasm` - WebAssembly binary (~7MB)
- `build-wasm/eita.html` - Browser test page

### Running with Node.js

```bash
cd build-wasm
node -e "const m = require('./eita.js'); m().then(() => console.log('Done!'))"
```

Example output:
```
[jank-wasm] jank WebAssembly Runtime
[jank-wasm] Module: eita
[jank-wasm] Calling jank_init...
[jank-wasm] Inside jank_main, runtime context should be initialized
[jank-wasm] Loading clojure.core-native...
[jank-wasm] Core native loaded!
[jank-wasm] Evaluating jank source...
[jank-wasm] Done!
Done!
```

### Running in Browser

```bash
# Start a local server
cd build-wasm
python3 -m http.server 8080

# Open in browser
open http://localhost:8080/eita.html
```

Or manually with Emscripten (advanced):

```bash
emcmake cmake -S . -B build-wasm -Djank_target_wasm=on
cmake --build build-wasm
em++ -o output.js output.cpp build-wasm/libjank.a [other libs...]
```

## Notes

- WASM support is experimental
- The full clojure.core is compiled alongside your code
- Generated code requires the jank runtime headers to compile
