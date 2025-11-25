# jank WASM Examples

This directory contains example jank programs for testing WebAssembly compilation.

## Running a .jank file with WASM AOT codegen

The `--codegen wasm_aot` flag generates C++ code that can be compiled by Emscripten for WebAssembly.

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
| `minimal.jank` | Minimal test - just returns 42 |
| `simple.jank` | Simple expressions |
| `loop_test.jank` | Tests loop/recur |
| `native_call.jank` | Tests native function calls |
| `eita.jank` | Full test with clojure.set and clojure.core |

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
./bin/emscripten-bundle your_file.jank
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
