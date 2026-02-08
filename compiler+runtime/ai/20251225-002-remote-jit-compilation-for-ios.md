# Remote JIT Compilation for iOS: Investigation and Implementation Plan

Date: 2025-12-25

## Executive Summary

**Question**: Can macOS perform JIT compilation and send results to iOS, instead of iOS doing JIT itself?

**Answer**: **YES!** macOS can cross-compile jank code to ARM64 object files and send them to iOS for direct execution. No interpretation needed - iOS loads and runs native code.

---

## The Core Problem (from 20251225-001)

When iOS JIT compiles code that calls inline C++ functions (e.g., `ImGui::Begin()`):

1. The inline function code gets JIT-compiled on iOS
2. The inline function references global variables (e.g., `GImGui`)
3. JIT can't find the AOT symbol → creates a **new** global variable
4. Result: JIT's `GImGui` ≠ AOT's `GImGui` → crash

**Root cause**: CppInterOp/Clang JIT on iOS creates duplicate symbols for `extern` variables it can't resolve.

---

## Current Architecture Analysis

### How eval_string Works (from `context.cpp:202-240`)

```cpp
object_ref context::eval_string(jtl::immutable_string_view const &code) {
    // 1. Lex (tokenize)
    read::lex::processor l_prc{ code };

    // 2. Parse (AST)
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

    for(auto const &form : p_prc) {
        // 3. Analyze (semantic analysis)
        analyze::processor an_prc;
        auto expr = an_prc.analyze(parsed_form, ...).expect_ok();

        // 4. Optimize
        expr = analyze::pass::optimize(expr);

        // 5. Evaluate (THIS IS WHERE JIT HAPPENS)
        ret = evaluate::eval(expr);
    }
    return ret;
}
```

### How evaluate::eval Works (from `evaluate.cpp`)

The key insight is that `evaluate::eval()` handles different expression types differently:

1. **Simple expressions** (primitives, maps, vectors, def, if, do): Evaluated directly via the AST interpreter
2. **Functions**: JIT compiled via codegen → CppInterOp (lines 646-722)
3. **C++ interop expressions**: Wrapped in function → JIT compiled (lines 1165-1240)

**Critical code path for functions** (`evaluate.cpp:688-717`):
```cpp
// C++ codegen path
codegen::processor cg_prc{ expr, module, codegen::compilation_target::eval };

// JIT compile the C++ code via CppInterOp
__rt_ctx->jit_prc.eval_string(cg_prc.declaration_str());

// Execute and get result
auto res(__rt_ctx->jit_prc.interpreter->ParseAndExecute(expr_str, &v));
```

This is where the problem occurs - when `cg_prc.declaration_str()` contains code that uses inline C++ functions.

---

## Proposed Solution: iOS nREPL Delegates Compilation to macOS

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        iOS Device                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Editor (CIDER/Calva) ──nREPL──► iOS nREPL Server          │
│                                        │                    │
│                                        │ eval op received   │
│                                        ↓                    │
│                         ┌──────────────────────────┐        │
│                         │  "I need this compiled"  │        │
│                         │  Send code to macOS ─────┼───┐    │
│                         └──────────────────────────┘   │    │
│                                                        │    │
│                                        ┌───────────────┘    │
│                                        │ receive .o         │
│                                        ↓                    │
│                         ┌──────────────────────────┐        │
│                         │  ORC JIT Object Loader   │        │
│                         │  • load_object(.o)       │        │
│                         │  • Resolve symbols       │        │
│                         │  • Execute native code!  │        │
│                         └──────────────────────────┘        │
│                                        │                    │
│                                        ↓                    │
│                              Return result to editor        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                                         │
                              TCP (compile request)
                                         │
┌────────────────────────────────────────│────────────────────┐
│                        macOS           │                     │
├────────────────────────────────────────│────────────────────┤
│                                        ↓                     │
│              ┌─────────────────────────────────┐            │
│              │  Compilation Service             │            │
│              │  ───────────────────────────     │            │
│              │  1. Receive jank source code     │            │
│              │  2. lex → parse → analyze        │            │
│              │  3. codegen → C++                │            │
│              │  4. Cross-compile to ARM64 .o    │            │
│              │     (with iOS PCH + flags)       │            │
│              │  5. Send .o back to iOS          │            │
│              └─────────────────────────────────┘            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Key Points

1. **iOS is the nREPL server** - Editor connects directly to iOS
2. **iOS delegates compilation** - When eval arrives, iOS asks macOS to compile
3. **macOS is a "compilation service"** - Just compiles, doesn't run nREPL
4. **iOS executes native code** - Loads object file, runs at full speed

### Key Insight: Why This Solves the Problem

**Current iOS JIT flow (broken):**
1. iOS receives jank source code
2. CppInterOp parses C++ headers (imgui.h)
3. CppInterOp sees `extern GImGui` → can't find symbol
4. CppInterOp creates NEW definition → duplicate global → crash

**New cross-compile flow (works):**
1. macOS receives jank source code
2. macOS compiles with **shared PCH** (same as iOS AOT)
3. macOS cross-compiles to ARM64 with proper symbol references
4. Object file has **relocations** for `GImGui` etc.
5. iOS loads object file via ORC JIT
6. ORC JIT resolves `GImGui` relocation → registered AOT address
7. Code runs with **correct** global → no crash!

### The Magic: load_object Already Exists!

From `jit/processor.cpp:404-429`:
```cpp
void processor::load_object(jtl::immutable_string_view const &path) const
{
    auto const ee{ interpreter->getExecutionEngine() };
    auto file{ llvm::MemoryBuffer::getFile(std::string_view{ path }) };
    // ...
    llvm::cantFail(ee->addObjectFile(std::move(file.get())));
}
```

ORC JIT can load pre-compiled object files! It handles symbol resolution automatically.

---

## Implementation Details

### Prerequisites on macOS

macOS needs to cross-compile for iOS. This requires:

1. **iOS SDK** (from Xcode)
2. **Clang with iOS target support** (already in jank's LLVM)
3. **Shared PCH** built for iOS (same one used for iOS AOT)
4. **Include paths** for native headers (imgui.h, etc.)

### Cross-Compilation Command

```bash
# macOS compiles jank-generated C++ to iOS ARM64 object
$LLVM_CLANG \
  -target arm64-apple-ios17.0 \
  -isysroot "$IOS_SDK" \
  -std=gnu++20 \
  -fPIC \
  -c \
  -include-pch /path/to/ios/incremental.pch \
  -I/path/to/imgui \
  -I/path/to/jank/include \
  -o output.o \
  generated_code.cpp
```

### Protocol Extension

The existing iOS remote eval protocol needs extension:

```cpp
// Current protocol (sends source code):
struct eval_request {
    std::string op = "eval";
    std::string code;      // jank source
    std::string ns;
    uint64_t id;
};

// New protocol (sends compiled object):
struct compiled_eval_request {
    std::string op = "eval-object";
    std::vector<uint8_t> object_data;  // ARM64 .o file contents
    std::string entry_symbol;           // Symbol to call after loading
    uint64_t id;
};
```

### iOS Object Loading

```cpp
// On iOS, receive and execute compiled object
object_ref execute_compiled_object(
    std::vector<uint8_t> const& object_data,
    std::string const& entry_symbol)
{
    // Create memory buffer from received data
    auto buffer = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<char const*>(object_data.data()),
                        object_data.size()),
        "remote_object",
        false /* RequiresNullTerminator */
    );

    // Load into ORC JIT
    auto const ee = __rt_ctx->jit_prc.interpreter->getExecutionEngine();
    llvm::cantFail(ee->addObjectFile(std::move(buffer)));

    // Find and call entry symbol
    auto fn_ptr = __rt_ctx->jit_prc.find_symbol(entry_symbol).expect_ok();
    return reinterpret_cast<object_ref(*)()>(fn_ptr)();
}
```

### Symbol Resolution: The Critical Part

For this to work, iOS must have **registered all AOT symbols** that the compiled code might reference:

```cpp
// During iOS app initialization (in jank_aot_init or similar)
void register_aot_symbols_for_jit() {
    auto const ee = __rt_ctx->jit_prc.interpreter->getExecutionEngine();
    auto& jd = ee->getMainJITDylib();

    // Register extern globals that inline functions use
    extern "C" ImGuiContext* GImGui;
    register_symbol(jd, "_GImGui", &GImGui);

    // Register jank runtime symbols
    register_symbol(jd, "_jank_nil", &runtime::jank_nil);
    // ... etc
}

void register_symbol(JITDylib& jd, char const* name, void* addr) {
    llvm::orc::SymbolMap symbols;
    symbols[jd.getExecutionSession().intern(name)] = {
        llvm::orc::ExecutorAddr::fromPtr(addr),
        llvm::JITSymbolFlags::Exported
    };
    llvm::cantFail(jd.define(llvm::orc::absoluteSymbols(symbols)));
}
```

---

## Complete Flow

### Step 1: User Types Code in Editor

```clojure
(defn my-render []
  (imgui/begin "Window")
  (imgui/text "Hello from nREPL!")
  (imgui/end))
```

### Step 2: Editor Sends to iOS nREPL Server

```
Editor (CIDER/Calva) ──nREPL──► iOS nREPL Server
```

### Step 3: iOS Requests Compilation from macOS

```cpp
// In iOS nREPL eval handler:
auto compile_result = request_compilation_from_macos(code, ns);

// Protocol message to macOS:
// { "op": "compile", "code": "(defn my-render ...)", "ns": "user" }
```

### Step 4: macOS Compiles and Returns Object

```cpp
// On macOS compilation service:

// 1. Lex, parse, analyze, optimize (normal jank pipeline)
auto expr = analyze_and_optimize(code);

// 2. Generate C++ code
codegen::processor cg_prc{ expr, module, codegen::compilation_target::eval };
auto cpp_code = cg_prc.declaration_str();

// 3. Cross-compile to ARM64 object (using iOS PCH!)
auto object_data = cross_compile_to_ios_arm64(cpp_code);

// 4. Return to iOS
return { .object_data = object_data,
         .entry_symbol = cg_prc.entry_symbol() };
```

### Step 5: iOS Loads Object and Executes

```cpp
// Back on iOS:

// 1. Receive compiled object from macOS
auto compiled = receive_from_macos();

// 2. Load into ORC JIT (symbol resolution happens here!)
auto buffer = llvm::MemoryBuffer::getMemBuffer(compiled.object_data, ...);
ee->addObjectFile(std::move(buffer));

// 3. Find and call entry function
auto fn_ptr = find_symbol(compiled.entry_symbol);
auto result = reinterpret_cast<object_ref(*)()>(fn_ptr)();

// 4. Return result to editor via nREPL response
return nrepl_response({ .value = to_string(result) });
```

### Step 6: Result Returns to Editor

```
iOS nREPL Server ──nREPL response──► Editor
```

---

## Why This Works (Symbol Resolution Deep Dive)

### The Problem with CppInterOp JIT

When CppInterOp parses `ImGui::Begin()`:

1. Clang sees `inline bool Begin(...) { ImGuiContext& g = *GImGui; ... }`
2. Clang needs address of `GImGui` to generate code
3. CppInterOp asks ORC JIT: "Where is `_GImGui`?"
4. ORC JIT: "Not found in symbol table"
5. CppInterOp: Creates tentative definition → **NEW** `GImGui` = NULL
6. **CRASH**

### How Cross-Compilation Fixes This

When macOS cross-compiles:

1. Clang sees same `inline bool Begin(...)`
2. Clang generates code with **relocation** for `_GImGui`
3. Object file contains: "At offset 0x1234, insert address of `_GImGui`"
4. iOS ORC JIT loads object file
5. ORC JIT processes relocations: "What's the address of `_GImGui`?"
6. **We registered it!** → ORC JIT patches in correct address
7. Code uses **AOT's** `GImGui` → **WORKS**

### Key Difference

| Aspect | CppInterOp JIT | Cross-Compiled Object |
|--------|----------------|----------------------|
| Symbol lookup | At parse time | At load time |
| Missing symbol | Creates new definition | Error (or uses registered) |
| Global variables | Duplicated | Shared with AOT |
| Header parsing | On iOS | On macOS |

---

## Files to Modify

### On macOS (Compilation Service)

| File | Change |
|------|--------|
| NEW: `src/cpp/jank/compile_server/server.cpp` | TCP server that accepts compile requests |
| NEW: `include/cpp/jank/compile_server/protocol.hpp` | Protocol definitions |
| NEW: `src/cpp/jank/cross_compile/ios.cpp` | Cross-compilation to ARM64 |

### On iOS (nREPL Server + Object Loader)

| File | Change |
|------|--------|
| `include/cpp/jank/nrepl_server/ops/eval.hpp` | Delegate to macOS instead of local JIT |
| NEW: `include/cpp/jank/compile_client/client.hpp` | Client to connect to macOS compile service |
| `src/cpp/jank/jit/processor.cpp` | Add `load_object_data()` for in-memory loading |
| iOS app code | Register AOT symbols on startup, connect to macOS |

---

## Implementation Plan

### Phase 1: macOS Compilation Service (3-4 days)

1. **Create compilation server**
   ```cpp
   // compile_server/server.hpp
   namespace jank::compile_server {
       class server {
       public:
           server(uint16_t port, std::string const& ios_pch_path);
           void start();  // Blocking, runs event loop

       private:
           void handle_compile_request(connection& conn, compile_request const& req);
       };
   }
   ```

2. **Implement cross-compilation**
   ```cpp
   // cross_compile/ios.hpp
   struct compile_result {
       std::vector<uint8_t> object_data;
       std::string entry_symbol;
       std::string error;
   };

   compile_result compile_for_ios(
       std::string const& jank_code,
       std::string const& ns,
       std::string const& pch_path);
   ```

3. **Protocol definition**
   ```cpp
   // Request: { "op": "compile", "code": "...", "ns": "user" }
   // Response: { "op": "compiled", "object": <base64>, "symbol": "..." }
   // Or error: { "op": "error", "message": "..." }
   ```

### Phase 2: iOS Compile Client + Object Loading (3-4 days)

1. **Connect iOS to macOS compile service**
   ```cpp
   // In iOS nREPL eval handler:
   if(macos_compile_client.is_connected()) {
       auto result = macos_compile_client.compile(code, ns);
       if(result.is_ok()) {
           return load_and_execute(result.object_data, result.entry_symbol);
       }
   }
   // Fallback to local JIT (may crash for inline C++ functions)
   ```

2. **In-memory object loading**
   ```cpp
   // jit/processor.cpp - add method:
   void processor::load_object_data(
       std::span<uint8_t const> object_data) const
   {
       auto buffer = llvm::MemoryBuffer::getMemBuffer(...);
       auto const ee = interpreter->getExecutionEngine();
       llvm::cantFail(ee->addObjectFile(std::move(buffer)));
   }
   ```

3. **Symbol registration on iOS startup**
   - Register AOT symbols before any eval

### Phase 3: Testing & Polish (2-3 days)

1. **Test with ImGui functions** (the original problem case)
2. **Error handling** - missing symbols, connection failures
3. **Graceful fallback** - if macOS unavailable, warn but try local JIT

---

## Challenges & Solutions

### Challenge 1: Matching Compilation Flags

macOS must compile with **exactly** the same flags as iOS AOT.

**Solution**: Store iOS compilation flags during AOT build, retrieve on macOS.

```cpp
// During iOS AOT build, save flags
void save_ios_compilation_flags(std::string const& flags_file) {
    std::ofstream out(flags_file);
    out << JANK_JIT_FLAGS << "\n";
    // ... other flags
}

// On macOS, read flags for cross-compilation
std::string load_ios_compilation_flags(std::string const& flags_file);
```

### Challenge 2: PCH Compatibility

The PCH must be built for iOS target, but used on macOS for cross-compilation.

**Solution**: Build iOS PCH as part of the iOS bundle, copy to macOS for dev.

```
iOS Bundle:
├── jank-resources/
│   ├── incremental.pch      ← Built for iOS ARM64
│   └── compile_flags.txt    ← Saved compilation flags
```

### Challenge 3: Symbol Discovery

How does iOS know which symbols to register?

**Solution 1**: Register all exported symbols from libjank.a
```cpp
// Use nm or llvm-nm to list symbols, register them all
```

**Solution 2**: Collect symbols during AOT build
```cpp
// Already implemented in runtime::context::collected_cpp_jit_symbols
```

**Solution 3**: On-demand registration with dlsym fallback
```cpp
// ORC JIT definition generator that calls dlsym
```

---

## Alternative: LLVM IR Instead of Object Files

Instead of compiling to object files, could send LLVM IR:

```cpp
// macOS generates LLVM IR
codegen::llvm_processor cg_prc{ expr, module, target::eval };
cg_prc.gen().expect_ok();
std::string ir = cg_prc.get_ir_string();

// Send IR to iOS
send_to_ios({ .op = "eval-ir", .ir_data = ir });

// iOS loads IR
auto module = llvm::parseIR(ir_data);
__rt_ctx->jit_prc.load_ir_module(std::move(module));
```

**Pros:**
- Smaller data transfer
- No external clang invocation
- Uses existing `load_ir_module()`

**Cons:**
- IR format is less stable than object files
- Need matching LLVM versions
- Still requires symbol resolution

**Verdict**: Object files are more robust, but IR is worth exploring.

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Clang invocation fails | Medium | High | Fallback to source-only mode |
| Symbol resolution fails | Medium | High | Comprehensive symbol registration |
| Object format mismatch | Low | High | Lock LLVM versions |
| Performance overhead | Low | Medium | Object caching |

---

## Implementation Timeline

| Week | Tasks |
|------|-------|
| 1 | Cross-compilation module, nREPL protocol extension |
| 2 | iOS object loading, symbol registration |
| 3 | Testing with ImGui, error handling, polish |

---

## Conclusion

This approach solves the iOS JIT symbol duplication problem by:

1. **iOS runs nREPL server** - Editor connects directly to iOS app
2. **iOS delegates compilation to macOS** - Sends jank code, receives ARM64 object
3. **macOS cross-compiles with iOS PCH** - Same headers, same symbols as AOT
4. **iOS loads object via ORC JIT** - Symbol resolution uses registered AOT addresses
5. **Native ARM64 execution** - No interpretation, full speed

The result: **Editor → iOS nREPL → (compile on macOS) → native execution on iOS**

No duplicate symbols, no crashes, native performance.

---

## References

- `src/cpp/jank/jit/processor.cpp:404-429` - load_object implementation
- `src/cpp/jank/jit/processor.cpp:431-443` - load_ir_module implementation
- `include/cpp/jank/nrepl_server/ios_remote_eval.hpp` - existing iOS remote eval
- ORC JIT symbol resolution: https://llvm.org/docs/ORCv2.html

---

## Implementation Status (2025-12-25)

### Files Created

| File | Description |
|------|-------------|
| `include/cpp/jank/compile_server/protocol.hpp` | Protocol definitions for compile requests/responses |
| `include/cpp/jank/compile_server/server.hpp` | macOS compilation server (TCP, cross-compiles to ARM64) |
| `include/cpp/jank/compile_server/client.hpp` | iOS compile client (connects to macOS) |
| `include/cpp/jank/compile_server/remote_eval.hpp` | iOS remote eval using compile client + ORC JIT |
| `src/cpp/compile_server_main.cpp` | macOS compile server main program |

### Modified Files

| File | Changes |
|------|---------|
| `include/cpp/jank/ios/eval_server.hpp` | Added remote compilation support via `enable_remote_compile()` |
| `include/cpp/jank/c_api.h` | Added iOS eval server C API functions |
| `src/cpp/jank/c_api.cpp` | Implemented iOS eval server C API |

### C API Functions

```c
// Start eval server on iOS without remote compilation
void jank_ios_start_eval_server(jank_u16 port);

// Start eval server with remote compilation to macOS
void jank_ios_start_eval_server_remote(jank_u16 eval_port,
                                       char const *compile_host,
                                       jank_u16 compile_port);

// Stop eval server
void jank_ios_stop_eval_server(void);

// Enable remote compilation on running server
void jank_ios_enable_remote_compile(char const *compile_host, jank_u16 compile_port);
```

### Usage

**On macOS** (development machine):
```bash
# Start the compile server
./compile-server --port 5559 --target sim
```

**On iOS** (called from jank code or app startup):
```cpp
// Option 1: Start with remote compilation from the beginning
jank_ios_start_eval_server_remote(5558, "192.168.1.100", 5559);

// Option 2: Enable remote compilation on existing server
jank_ios_start_eval_server(5558);
jank_ios_enable_remote_compile("192.168.1.100", 5559);
```

**From editor** (CIDER, Calva, etc.):
```
Connect to iOS-device-IP:5558 as nREPL server
Evaluate (defn foo [] (ImGui::Begin "Test"))  ; Compiled on macOS, runs on iOS!
```

### How It Works

1. Editor sends eval request to iOS nREPL server (port 5558)
2. iOS nREPL receives the code and sends it to macOS compile server (port 5559)
3. macOS:
   - Parses and analyzes the jank code
   - Generates C++ code
   - Cross-compiles to ARM64 object file using iOS SDK
   - Returns object file (base64 encoded) + entry symbol
4. iOS:
   - Receives object file bytes
   - Loads into ORC JIT via `addObjectFile()`
   - Calls entry symbol to execute
   - Returns result to editor

### Why This Works

The key insight is that **CppInterOp creates tentative definitions at parse time**, while **cross-compiled objects have relocations that get resolved at load time**.

When macOS compiles the code:
- It uses the same iOS PCH as the AOT build
- Generated object has relocations to symbols like `GImGui`
- No tentative definitions are created

When iOS loads the object:
- ORC JIT resolves relocations using registered symbols
- `dlsym(RTLD_DEFAULT, "GImGui")` finds the AOT symbol
- Relocation is patched with the correct address

Result: Same `GImGui` as AOT code - no duplicate symbols, no crashes!
