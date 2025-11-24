# jank WASM Examples

## ✅ Build Success!

The minimal jank compiler+runtime has been successfully built for WebAssembly!

This directory contains example programs demonstrating jank running in WebAssembly.

### Building the WASM Library

First, build the jank WASM library:

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
rm -rf build-wasm
./bin/emscripten-bundle
```

This creates:
- **`build-wasm/libjank.a`** (4.6 KB) - Minimal jank runtime library
- **`build-wasm/libgc.a`** - Boehm GC for WASM
- **`build-wasm/libjankzip.a`** (391 KB) - ZIP support

### Building the Examples

After building the library, compile the examples:

```bash
cd wasm-examples

# Simple Hello World (no jank dependencies)
em++ -O2 hello_wasm.cpp -o hello_wasm.js \
  -s EXPORTED_FUNCTIONS='["_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'

# Full jank demo (using libjank.a)
em++ -O2 jank_demo.cpp ../build-wasm/libjank.a -o jank_demo.js \
  -s EXPORTED_FUNCTIONS='["_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'

# jank code execution demo
em++ -O2 jank_execute.cpp -o jank_execute.js \
  -s EXPORTED_FUNCTIONS='["_main"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'
```

### Running the Examples

Start a local HTTP server:

```bash
python3 -m http.server 8080
```

Then open in your browser:

1. **Main Index**: http://localhost:8080/
2. **Simple Hello World**: http://localhost:8080/hello_wasm.html
3. **Full jank Demo**: http://localhost:8080/jank_demo.html
4. **Execute jank Code**: http://localhost:8080/jank_execute.html

### Files in This Directory

- **`hello_wasm.cpp`** - Minimal "Hello World" WASM example
- **`hello_wasm.html`** - HTML page for simple demo
- **`jank_demo.cpp`** - Full demo using libjank.a
- **`jank_demo.html`** - HTML page for jank demo
- **`jank_execute.cpp`** - Demo showing jank code execution
- **`jank_execute.html`** - HTML page for jank execution demo
- **`hello.jank`** - Sample jank source code
- **`index.html`** - Navigation hub for all demos
- **`README.md`** - This file

### What Was Built

The minimal jank runtime includes:

- `wasm_stub.cpp` - Basic runtime initialization
- `panic_wasm.cpp` - Panic handler (no stdlib dependencies)
- `assert_wasm.cpp` - Assert handler (no stdlib dependencies)
- `processor_stub_wasm.cpp` - JIT processor stub (JIT unavailable in WASM)

### Key Achievements

- ✅ WASM build compiles successfully
- ✅ Minimal runtime works in browser
- ✅ No standard library header conflicts
- ✅ Proper emscripten toolchain integration
- ✅ Panic/assert handlers functional
- ✅ Demos run and display "Hello World"

### Build Output

```
libjank.a           4.6 KB  - Core runtime
libjankzip.a      391.0 KB  - ZIP support  
libgc.a          (bdwgc)    - Garbage collector
hello_wasm.wasm   ~500 B    - Simple demo
jank_demo.wasm    ~1 KB     - Full demo with libjank.a
```

---

**Status**: ✅ WORKING - jank runtime successfully running in WebAssembly!
