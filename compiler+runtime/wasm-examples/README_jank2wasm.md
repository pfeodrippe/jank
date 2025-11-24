# jank2wasm - Compile jank to WebAssembly

Simple script to compile jank code to WebAssembly and run it with Node.js.

## Usage

```bash
./jank2wasm.sh <jank-file>
```

## Example

```bash
./jank2wasm.sh hello.jank
```

Output:
```
🚀 Compiling jank → WASM → Node.js
====================================

📝 Step 1/3: Analyzing jank code...
   Namespace: hello
🔧 Step 2/3: Generating C++ code...
   ✓ Generated hello_wasm.cpp
⚙️  Step 3/3: Compiling to WASM...
   ✓ Compiled to hello.{js,wasm}

✅ Build complete!

📦 Running with Node.js...
====================================

Hello World

====================================
✓ Execution complete!
```

## What it does

1. **Analyzes** your jank code to understand what it does
2. **Generates** C++ code that implements the same logic
3. **Compiles** the C++ to WebAssembly using emscripten
4. **Runs** the result with Node.js

## Requirements

- **emscripten** - Install from https://emscripten.org/docs/getting_started/downloads.html
- **Node.js** - For running the compiled WASM

## Generated files

- `<name>.js` - JavaScript glue code for Node.js
- `<name>.wasm` - WebAssembly binary

## Supported jank features

Currently supports:
- `(println "...")` statements
- Multiple println calls
- Namespaces

## Example jank code

```clojure
(ns hello)

(defn -main []
  (println "Hello World"))
```

## Notes

This is a simplified proof-of-concept that demonstrates the jank → WASM pipeline works. The script parses your jank code and generates equivalent C++ that compiles to WASM.

For full runtime support (dynamic features, vars, objects, etc.), more work is needed to build the complete jank runtime for WebAssembly.
