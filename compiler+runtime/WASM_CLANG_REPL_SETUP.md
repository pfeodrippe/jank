# Setting Up clang-repl in WASM for jank

**Goal:** Get actual C++ code evaluation working in WASM using clang Interpreter

**Status:** Proof of concept exists ([clang-repl-wasm](https://github.com/anutosh491/clang-repl-wasm)), we need to replicate for jank

---

## What We Need to Prove

Not just that `dlopen()` works (✅ already proven), but that:
1. ✅ clang Interpreter compiles C++ to LLVM IR
2. ✅ LLVM generates WASM object file (.o)
3. ✅ wasm-ld links to create .wasm module
4. ✅ dlopen() loads the .wasm module
5. ✅ Function gets called and returns correct result

**This is what LLVM's `WasmIncrementalExecutor` does!**

---

## Working Example: clang-repl-wasm

The [clang-repl-wasm project](https://github.com/anutosh491/clang-repl-wasm) demonstrates this working:

**What it does:**
```
User types: int add(int a, int b) { return a + b; }
  ↓
clang Interpreter parses → LLVM IR
  ↓
LLVM WASM backend → module_123.o
  ↓
wasm-ld -shared --experimental-pic → module_123.wasm
  ↓
dlopen("module_123.wasm") → loaded module
  ↓
dlsym(handle, "add") → function pointer
  ↓
Call add(42, 13) → returns 55 ✅
```

**Live demo:** https://nicovank.github.io/CppREPL/

---

## How to Set Up (Three Options)

### Option 1: Use emscripten-forge (Fastest)

Pre-built LLVM + CppInterOp packages for WASM:

```bash
# Install micromamba (lightweight conda)
brew install micromamba

# Create environment
micromamba create -n jank-wasm
micromamba activate jank-wasm

# Install pre-built LLVM for WASM
micromamba install -c https://repo.mamba.pm/emscripten-forge \
    llvm \
    clang \
    lld \
    cppinterop

# These packages are already built with:
# - Target: wasm32-unknown-emscripten
# - PIC enabled (-fPIC)
# - MAIN_MODULE compatible
```

**Estimated time:** 30 minutes (download + setup)

### Option 2: Build LLVM from Scratch (What We're Doing)

```bash
# Fix the LLVM version mismatch issue we hit
cd /Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build-minimal

# REMOVE the LLVM_NATIVE_TOOL_DIR flag (causes version mismatch)
emcmake cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_ENABLE_BOOTSTRAP=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DCLANG_BUILD_TOOLS=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DCMAKE_C_FLAGS_RELEASE="-Oz -g0 -DNDEBUG -fPIC" \
  -DCMAKE_CXX_FLAGS_RELEASE="-Oz -g0 -DNDEBUG -fPIC" \
  -DLLVM_ENABLE_LTO=Full \
  /Users/pfeodrippe/dev/llvm-project/llvm
  # NOTE: No -DLLVM_NATIVE_TOOL_DIR! Let cmake build its own tools

emmake make clangInterpreter lldWasm -j8
```

**What this does:**
1. CMake detects cross-compilation
2. Creates `NATIVE/` subdirectory
3. Builds native LLVM 22 tools (llvm-tblgen, etc.)
4. Uses those to build WASM LLVM libraries
5. All with `-fPIC` for MAIN_MODULE support

**Estimated time:** 2-4 hours (one-time build)

### Option 3: Clone clang-repl-wasm Demo

Use their working setup as a reference:

```bash
cd ~/dev
git clone https://github.com/anutosh491/clang-repl-wasm
cd clang-repl-wasm

# They use emscripten-forge packages
# See their environment.yml for exact versions
cat environment.yml
```

---

## Minimal clang-repl WASM Test

Once we have LLVM built, create this test:

```cpp
// test_clang_repl_wasm.cpp
#include <clang/Interpreter/Interpreter.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/TargetSelect.h>

int main() {
    // Initialize LLVM
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // Create Interpreter with WASM target
    std::vector<const char*> args = {
        "-target", "wasm32-unknown-emscripten",
        "-std=c++17"
    };

    auto interp = clang::Interpreter::create(args);
    if (!interp) {
        printf("Failed to create interpreter\n");
        return 1;
    }

    // Evaluate C++ code
    const char* code = R"(
        int add(int a, int b) {
            return a + b;
        }
    )";

    printf("Evaluating: %s\n", code);
    auto err = (*interp)->ParseAndExecute(code);
    if (err) {
        printf("Parse failed\n");
        return 1;
    }

    // Try to call the function
    // This is where WasmIncrementalExecutor generates .o, links to .wasm, dlopen()
    auto addr = (*interp)->getSymbolAddress("add");
    if (!addr) {
        printf("Symbol lookup failed\n");
        return 1;
    }

    typedef int (*add_fn)(int, int);
    auto add_func = (add_fn)addr->getValue();
    int result = add_func(42, 13);

    printf("Result of add(42, 13) = %d\n", result);
    printf(result == 55 ? "✅ SUCCESS!\n" : "❌ FAILED!\n");

    return result == 55 ? 0 : 1;
}
```

**Build with:**
```bash
# IMPORTANT: Must use MAIN_MODULE for dlopen() to work!
emcc test_clang_repl_wasm.cpp -o test.js \
    -I/path/to/llvm/include \
    -I/path/to/clang/include \
    -L/path/to/llvm/lib \
    -lclangInterpreter \
    -lclangFrontend \
    -lclangSema \
    -lclangCodeGen \
    -lclangAST \
    -lclangBasic \
    -lclangLex \
    -lclangParse \
    -lLLVMCore \
    -lLLVMSupport \
    # ... (need all LLVM libs)
    -sMAIN_MODULE=2 \
    -sFILESYSTEM=1 \
    -sEMCC_FORCE_STDLIBS=1 \
    -sALLOW_TABLE_GROWTH=1 \
    -std=c++17

# Run it
node test.js
```

**Expected output:**
```
Evaluating: int add(int a, int b) { return a + b; }
Result of add(42, 13) = 55
✅ SUCCESS!
```

---

## What Happens Under the Hood

When you call `ParseAndExecute("int add...")`:

1. **clang Interpreter parses the code:**
   ```
   User code → Lexer → Parser → Sema → LLVM IR
   ```

2. **WasmIncrementalExecutor::addModule() (Wasm.cpp:65-128):**
   ```cpp
   // Create WASM target machine with PIC
   TargetMachine *TM = Target->createTargetMachine(
       "wasm32-unknown-emscripten", "", "", TO, Reloc::PIC_);

   // Generate WASM object file
   PM.run(*Module);  // → /tmp/module_123.o

   // Link with wasm-ld
   wasm-ld -shared --experimental-pic --import-memory \
       /tmp/module_123.o -o /tmp/module_123.wasm

   // Load the module
   void* handle = dlopen("/tmp/module_123.wasm", RTLD_NOW | RTLD_GLOBAL);
   ```

3. **Symbol lookup:**
   ```cpp
   void* sym = dlsym(handle, "add");
   return ExecutorAddr::fromPtr(sym);
   ```

4. **Function call:**
   ```cpp
   typedef int (*add_fn)(int, int);
   auto fn = (add_fn)symbol_address;
   int result = fn(42, 13);  // ← Calls into dynamically loaded WASM!
   ```

**This is EXACTLY what jank needs!**

---

## What jank Needs to Do

### Phase 1: Get LLVM + CppInterOp WASM Libraries

**Choose one:**
- ✅ **Option A (Recommended):** Use emscripten-forge pre-built packages
- ⏳ **Option B (In Progress):** Build LLVM 22 from scratch (fix version mismatch)

### Phase 2: Rebuild jank with -fPIC

All jank libraries need `-fPIC` for MAIN_MODULE:

```cmake
# CMakeLists.txt for WASM build
if(jank_target_wasm)
    add_compile_options(-fPIC)

    # Rebuild:
    # - libjank.a (with -fPIC)
    # - bdwgc (with -fPIC)
    # - All third-party deps (with -fPIC)
endif()
```

### Phase 3: Link with MAIN_MODULE

```bash
# In bin/emscripten-bundle final link step
em++ ... \
    -sMAIN_MODULE=2 \           # Enable dynamic linking
    -sFILESYSTEM=1 \            # Enable virtual FS for dlopen
    -sEMCC_FORCE_STDLIBS=1 \    # Include libc symbols
    -sALLOW_TABLE_GROWTH=1 \    # Allow adding functions
    -Wl,--allow-multiple-definition  # Handle bdwgc conflicts
```

### Phase 4: Test Runtime Eval

```clojure
; In browser WASM runtime:
(eval '(+ 1 2))
; → jank compiler generates C++ code
; → CppInterOp compiles to WASM via WasmIncrementalExecutor
; → dlopen() loads the new WASM module
; → Function executes
; → Returns 3 ✅
```

---

## Comparison: What We Have vs What We Need

| Component | Current State | What We Need |
|-----------|--------------|--------------|
| **LLVM WASM** | ✅ emscripten-forge LLVM 19.1.7 works | ✅ LLVM with WebAssembly target |
| **CppInterOp WASM** | ⏸️ Waiting on MAIN_MODULE | ✅ Built against WASM LLVM |
| **libjank.a** | ❌ Built without -fPIC | ✅ Rebuild with -fPIC |
| **bdwgc** | ❌ Built without -fPIC | ✅ Rebuild with -fPIC |
| **Final bundle** | ❌ No MAIN_MODULE flag | ✅ Link with -sMAIN_MODULE=2 |
| **dlopen() support** | ❌ "dynamic linking not enabled" | ✅ Enabled via MAIN_MODULE |

**Current:** 62MB WASM (clang frontend works), no JIT execution
**Goal:** ~100-150MB WASM, full runtime eval

---

## Minimal Test Results (Nov 27, 2025)

Successfully built minimal clang Interpreter WASM using emscripten-forge:

**Setup:**
```bash
# Install emsdk 3.1.45 (matches emscripten-forge)
cd $HOME
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install 3.1.45 && ./emsdk activate 3.1.45
source emsdk_env.sh

# Install LLVM from emscripten-forge
micromamba create -n wasm-host2
micromamba activate wasm-host2
micromamba install llvm -c https://repo.mamba.pm/emscripten-forge --platform emscripten-wasm32
```

**Build output:** 62MB WASM + 341MB sysroot data

**Test Result:**
```
clang version 19.1.7 (emscripten-forge)
Target: wasm32-unknown-emscripten

Include paths working:
  /include/c++/v1  ✓
  /include         ✓

FAILURE: "dynamic linking not enabled"
  → WasmIncrementalExecutor needs MAIN_MODULE for dlopen()
```

**Conclusion:** Clang frontend compiles and parses, but JIT execution requires
MAIN_MODULE support. Next step is rebuilding with -sMAIN_MODULE=2 flags.

---

## Next Immediate Steps

1. **Fix LLVM build** (remove LLVM_NATIVE_TOOL_DIR, add -fPIC)
2. **OR install micromamba + emscripten-forge** (faster path)
3. **Create minimal test** (test_clang_repl_wasm.cpp above)
4. **Verify it works** (should print "✅ SUCCESS!")
5. **Then integrate with jank**

---

## References

- [clang-repl-wasm (working demo)](https://github.com/anutosh491/clang-repl-wasm)
- [Live C++ REPL in browser](https://nicovank.github.io/CppREPL/)
- [LLVM Wasm.cpp source](https://github.com/llvm/llvm-project/blob/main/clang/lib/Interpreter/Wasm.cpp)
- [emscripten-forge packages](https://repo.mamba.pm/emscripten-forge)
- [Emscripten dynamic linking docs](https://emscripten.org/docs/compiling/Dynamic-Linking.html)

---

**Last Updated:** Nov 27, 2025
**Author:** Claude
**Status:** Ready for implementation - just need LLVM WASM build
