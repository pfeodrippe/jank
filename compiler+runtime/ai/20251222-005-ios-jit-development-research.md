# iOS JIT for Development - Research & Implementation Plan

**Date**: 2025-12-22
**Status**: Research Complete, Implementation Options Identified
**Updated**: Added clang-repl/cling WebAssembly research and LLVM-On-iOS analysis

## Executive Summary

iOS has strict code signing requirements that prevent traditional JIT compilation on physical devices. However, there are several viable approaches for enabling interactive development with jank on iOS, ranging from simulator-only JIT to remote compilation with hot-reload.

**Key Discovery**: The clang-repl project has successfully run in the browser via WebAssembly using a `WasmIncrementalExecutor` that bypasses traditional JIT. This same technique could potentially work on iOS!

## Table of Contents

1. [iOS JIT Landscape](#1-ios-jit-landscape)
2. [How jank JIT Works](#2-how-jank-jit-works)
3. [iOS Simulator vs Device Differences](#3-ios-simulator-vs-device-differences)
4. [Clang-Repl in WebAssembly - Key Breakthrough](#4-clang-repl-in-webassembly---key-breakthrough)
5. [LLVM on iOS - Existing Projects](#5-llvm-on-ios---existing-projects)
6. [Available Approaches](#6-available-approaches)
7. [Recommended Implementation Plan](#7-recommended-implementation-plan)
8. [Technical Details](#8-technical-details)
9. [Sources](#9-sources)

---

## 1. iOS JIT Landscape

### Apple's Restrictions

Apple prohibits third-party apps from generating executable code at runtime on iOS physical devices due to security concerns:

- **Code Signing**: All executable code must be signed by Apple or a trusted developer certificate
- **Memory Protection**: Apps cannot create memory pages that are both writable and executable (`W^X` policy)
- **No `dynamic-codesigning` Entitlement**: Only Apple's system processes (Safari/WebKit) can use the `MAP_JIT` flag

### Security Rationale

Based on CVE data since 2019:
- ~45% of V8 JavaScript engine vulnerabilities were JIT-related
- More than half of all "in the wild" Chrome exploits abused JIT bugs
- JIT allows arbitrary code execution if compromised

### Historical Workarounds (Now Mostly Patched)

| Method                   | iOS Versions            | Status                                 |
|--------------------------|-------------------------|----------------------------------------|
| PT_TRACE_ME ptrace trick | iOS 13 and earlier      | **Patched in iOS 14**                  |
| SideJITServer            | iOS 17.0 - 18.3         | Works, requires Mac on same WiFi       |
| StikDebug/StikJIT        | iOS 17.4 - 18.3         | Works, not on iOS 18.4+                |
| AltJIT/AltStore          | iOS 17+                 | Requires USB connection to Mac         |
| TrollStore               | iOS 17.0 - 17.0.5       | Limited version support                |
| BrowserEngineKit         | iOS 17+ (EU/Japan only) | Requires Apple approval, EU/Japan only |
|                          |                         |                                        |

### iOS 18.4+ Changes (2025)

Apple introduced stricter requirements:
- Requires `CS_DEBUGGED` flag AND debugger must have `com.apple.private.cs.debugger` entitlement
- Only Xcode's debugger has this entitlement
- SideJITServer and similar tools no longer work on iOS 18.4+

---

## 2. How jank JIT Works

### Core Components

jank uses LLVM's ORC JIT framework with Clang's CppInterOp interpreter:

```
┌─────────────────────────────────────────────────────────────────┐
│                     jank JIT Processor                          │
├─────────────────────────────────────────────────────────────────┤
│  Cpp::Interpreter (CppInterOp)                                  │
│    └── clang::Interpreter                                       │
│          └── LLVM ORC JIT (LLJIT)                               │
│                ├── ThreadSafeModule loading                     │
│                ├── Symbol management (find, remove)             │
│                ├── Dynamic library loading                      │
│                └── Object file loading                          │
└─────────────────────────────────────────────────────────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `src/cpp/jank/jit/processor.cpp` | Full JIT implementation (~550 lines) |
| `include/cpp/jank/jit/processor.hpp` | JIT processor interface |
| `src/cpp/jank/jit/processor_stub.cpp` | Emscripten/iOS stub (throws errors) |
| `src/cpp/jank/evaluate.cpp` | Code evaluation entry point |

### JIT Processor Constructor Flow

```cpp
processor::processor(jtl::immutable_string const &binary_version) {
  // 1. Parse JIT flags from JANK_JIT_FLAGS
  // 2. Find clang and resource directories
  // 3. Load precompiled header (PCH)
  // 4. Create CppInterOp interpreter with LLVM CodeModel::Large
  // 5. Install fatal error handler for REPL recovery
  // 6. Optionally enable perf profiling plugins
  // 7. Load dynamic libraries from CLI options
}
```

### iOS/WASM Build Strategy

For iOS, jank reuses the WASM AOT approach by defining:
```cpp
JANK_TARGET_IOS=1
JANK_TARGET_WASM=1
JANK_TARGET_EMSCRIPTEN=1
```

This causes `processor_stub.cpp` to be used, which throws "JIT is unavailable when targeting emscripten" for all JIT operations.

---

## 3. iOS Simulator vs Device Differences

### Memory Protection

| Platform | Executable Memory | JIT Allowed |
|----------|-------------------|-------------|
| iOS Simulator (macOS host) | RWX pages allowed | **Yes** |
| iOS Device (Debug + Debugger) | Limited via CS_DEBUGGED | Partially |
| iOS Device (Release) | No executable memory generation | **No** |

### Key Insight

**The iOS Simulator runs on macOS and inherits macOS memory protection rules**, which are more permissive:
- Simulator can toggle between RW (read-write) and RX (read-execute) memory pages
- This is exactly what JIT needs for hot-reloading

### Flutter's Approach

Flutter demonstrates this pattern:
- **Debug mode**: Uses Dart VM with JIT for hot reload (simulator only on iOS 26+)
- **Release mode**: Uses AOT compilation
- iOS 26 broke JIT on physical devices entirely, forcing simulator-only debug builds

---

## 4. Clang-Repl in WebAssembly - Key Breakthrough

### The Problem: JIT in Sandboxed Environments

WebAssembly operates in a sandboxed Harvard architecture where code and data reside in completely distinct memory spaces. This makes conventional JIT compilation unfeasible - you cannot dynamically modify executable memory.

**This is the exact same problem as iOS!**

### The Solution: WasmIncrementalExecutor

The LLVM/Clang team solved this by creating a new execution model that **bypasses JIT entirely**:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    WasmIncrementalExecutor Flow                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. Parse input → Partial Translation Unit (PTU)                            │
│                        ↓                                                    │
│  2. Lower to LLVM IR                                                        │
│                        ↓                                                    │
│  3. Compile to .o file (WebAssembly object file)                            │
│           ↓                                                                 │
│  4. Link with wasm-ld → standalone .wasm module                             │
│           ↓                                                                 │
│  5. dlopen() the .wasm module (shares memory with main module)              │
│           ↓                                                                 │
│  6. Symbols become available via dlsym()                                    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Technical Implementation Details

From `clang/lib/Interpreter/Wasm.cpp`:

```cpp
// WasmIncrementalExecutor::addModule(PartialTranslationUnit &PTU)

// 1. Get WebAssembly target
const llvm::Target *Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

// 2. Create TargetMachine with PIC relocation
std::unique_ptr<llvm::TargetMachine> TM = Target->createTargetMachine(
    TargetTriple, "", "", Options, Reloc::PIC_);

// 3. Generate .o file via LLVM WebAssembly backend
llvm::legacy::PassManager PM;
PM.add(llvm::createTargetTransformInfoWrapperPass(TM->getTargetIRAnalysis()));
TM->addPassesToEmitFile(PM, ObjStream, nullptr, CodeGenFileType::ObjectFile);

// 4. Link with wasm-ld
// Flags: -shared, --import-memory, --experimental-pic,
//        --stack-first, --allow-undefined, --export-all
lld::wasm::link(LinkerArgs, ...);

// 5. Load via dlopen
void *Handle = dlopen(WasmPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
```

### Key Insight for jank

**The same approach could work for iOS!** Instead of:
- WebAssembly backend → iOS ARM64 backend
- wasm-ld → ld64 (Apple's linker)
- .wasm modules → .dylib modules

The execution model remains the same: compile each REPL input to a shared library, then dlopen() it.

### xeus-cpp-lite: C++ in Browser

This approach powers [xeus-cpp-lite](https://compiler-research.org/xeus-cpp/lab/index.html), a fully client-side C++ Jupyter kernel:

- Runs entirely in the browser
- No server needed
- Full C++ REPL with incremental compilation
- Uses Emscripten + LLVM + wasm-ld compiled to WebAssembly

### Known Limitations

- **No throw/catch**: Exception handling doesn't work in browser WebAssembly
- **Memory limits**: Browser WebAssembly has memory constraints
- **Startup time**: Initial load of LLVM/Clang in WASM is slow (~seconds)

---

## 5. LLVM on iOS - Existing Projects

### LLVM-On-iOS Project

[light-tech/LLVM-On-iOS](https://github.com/light-tech/LLVM-On-iOS) demonstrates running LLVM on iOS:

**What Works**:
- Clang's C interpreter example runs on iOS
- Compiles and executes C++ code in-app
- Compilation errors displayed in UI

**What Doesn't Work**:
- JIT execution on physical devices (SIGTERM CODESIGN)
- Only works from Xcode (debugger provides JIT permission)
- Cannot run standalone on real devices

**Workaround Mentioned**:
> "Use the slower LLVM bytecode interpreter instead of ORC JIT"

### a-Shell: Production App on App Store

[a-Shell](https://holzschu.github.io/a-Shell_iOS/) is a **production iOS app** that runs C/C++ code:

**How It Works**:
```
┌─────────────────────────────────────────────────────────────────┐
│                    a-Shell C/C++ Workflow                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  C/C++ Source                                                   │
│       ↓                                                         │
│  clang/clang++ (compiled for iOS)                               │
│       ↓                                                         │
│  WebAssembly bytecode (.wasm)                                   │
│       ↓                                                         │
│  wasm3 interpreter (or wasm interpreter)                        │
│       ↓                                                         │
│  Execution result                                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Key Points**:
- Uses WASI (WebAssembly System Interface) for I/O
- Includes wasm3 interpreter (fast WebAssembly interpreter)
- **App Store approved** - fully compliant with Apple policies
- No JIT needed - pure interpretation of WASM bytecode
- LLVM/Clang 18.1.5 included

**Limitations**:
- No sockets, no forks
- No pthreads (WebAssembly limitation)
- Performance is interpreter-level (not native)

### App Store C++ IDEs

Multiple apps use LLVM/Clang for C++ on iOS:

| App                    | Approach                  |
|------------------------|---------------------------|
| C/C++ LLVM Clang       | LLVM + local execution    |
| Mobile C               | Multi-language with LLVM  |
| C++ Shell              | LLVM + Clang compilation  |
| C/C++ Offline Compiler | ClangFormat + compilation |
|                        |                           |

All use some form of interpretation or bytecode execution rather than native JIT.

---

## 6. Available Approaches

### Approach 1: Simulator-Only JIT ⭐ RECOMMENDED FOR QUICK WIN

**Concept**: Enable full JIT on iOS Simulator, keep AOT for device builds.

**Pros**:
- Simulator runs on macOS with full JIT capability
- No Apple restrictions
- Fastest to implement
- Same development experience as macOS native

**Cons**:
- Can't debug JIT-compiled code on physical device
- May have subtle behavior differences from device

**Implementation**:
1. Create new build configuration: `jank_target_ios_simulator=ON`
2. Simulator build uses full `processor.cpp` (not stub)
3. Device build continues using `processor_stub.cpp`
4. Build script auto-detects target (simulator vs device)

```cmake
if(jank_target_ios AND CMAKE_OSX_SYSROOT MATCHES "Simulator")
  # Use full JIT processor
  set(JIT_SOURCES src/cpp/jank/jit/processor.cpp)
else()
  # Use stub for device
  set(JIT_SOURCES src/cpp/jank/jit/processor_stub.cpp)
endif()
```

---

### Approach 2: Remote JIT via nREPL + Hot Reload ⭐ RECOMMENDED FOR DEVICE

**Concept**: JIT compilation happens on Mac, compiled code is sent to device.

```
┌─────────────────┐         nREPL          ┌──────────────────┐
│   Mac (Host)    │◄─────────────────────►│   iOS Device     │
│                 │                        │                  │
│  jank compiler  │   1. Send code         │  jank runtime    │
│  + JIT          │   2. Compile on Mac    │  (AOT only)      │
│                 │   3. Send .o file      │                  │
│                 │   4. Load & execute    │  dlopen() new    │
│                 │                        │  code module     │
└─────────────────┘                        └──────────────────┘
```

**Key Insight**: iOS allows `dlopen()` of code **signed at build time**. The workflow:

1. Developer edits code on Mac
2. Mac compiles to iOS-compatible object file
3. Object file is pre-signed during development
4. Object file sent to device via nREPL
5. Device loads via dlopen (if signed) or other mechanism

**Challenges**:
- Need to pre-sign all code during development
- Or use on-device code loading that doesn't require new signatures

**Alternative: Interpreted Hot Reload**

Instead of sending compiled code, send the jank source:
1. Developer edits jank code
2. Code sent to device via nREPL
3. Device re-evaluates using pre-compiled clojure.core
4. Only works for logic changes, not native interop changes

---

### Approach 3: Development-Time JIT via SideJITServer (iOS ≤18.3)

**Concept**: Use existing JIT-enabling tools during development.

**Workflow**:
1. Install app via Xcode (with `get-task-allow` entitlement)
2. Run SideJITServer on Mac
3. Enable JIT for the app
4. Full JIT works until app goes to background

**Pros**:
- Full native JIT on device
- No code changes to jank

**Cons**:
- Doesn't work on iOS 18.4+
- JIT lost when app backgrounds
- Requires additional tooling setup
- Not viable for iOS 26+

---

### Approach 4: BrowserEngineKit (EU/Japan Only, Requires Apple Approval)

**Concept**: Use Apple's official JIT APIs for browser engines.

**Reality Check**:
- Only available in EU and Japan
- Requires special entitlement from Apple
- Designed for web browsers, not general apps
- Multi-process architecture mandatory
- **Not practical for jank**

---

### Approach 5: LLVM Interpreter Fallback

**Concept**: When JIT unavailable, fall back to LLVM IR interpretation.

**How it could work**:
1. Generate LLVM IR as usual
2. Instead of JIT compiling to native, interpret the IR
3. LLVM has an interpreter mode (`lli -force-interpreter`)

**Pros**:
- Same code path for analysis/compilation
- Works everywhere

**Cons**:
- Much slower than native execution
- Significant implementation effort
- LLVM interpreter not optimized for interactive use

---

### Approach 6: Hermes-Style AOT Bytecode Interpreter

**Concept**: Design a jank bytecode format optimized for interpretation.

**How React Native/Hermes does it**:
- JavaScript compiled to Hermes bytecode at build time
- Bytecode interpreter runs on device
- No JIT needed - fast startup, acceptable performance

**For jank**:
1. Define jank bytecode format
2. Compile jank to bytecode (not C++)
3. Bytecode interpreter in C++
4. Could hot-reload bytecode

**Pros**:
- Fast startup
- Hot-reload capable
- No platform restrictions

**Cons**:
- Massive implementation effort (new compiler backend)
- Loses C++ interop advantages
- Performance won't match native

---

### Approach 7: iOS Incremental Executor (WasmIncrementalExecutor Pattern) ⭐⭐ MOST PROMISING

**Concept**: Adapt the WasmIncrementalExecutor approach for iOS - compile each REPL input to a dylib, dlopen() it.

**How It Would Work**:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    iOS Incremental Executor Flow                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. Parse jank input → Generate C++                                         │
│                        ↓                                                    │
│  2. Compile C++ → LLVM IR (using embedded Clang)                            │
│                        ↓                                                    │
│  3. Lower IR → ARM64 object file (.o)                                       │
│           ↓                                                                 │
│  4. Link with ld64 → shared library (.dylib)                                │
│           ↓                                                                 │
│  5. Code-sign the .dylib (ad-hoc or dev certificate)                        │
│           ↓                                                                 │
│  6. dlopen() the .dylib                                                     │
│           ↓                                                                 │
│  7. Look up symbols via dlsym(), execute                                    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Key Challenge: Code Signing**

iOS requires all executable code to be signed. Options:

1. **Ad-hoc signing on device** (during development from Xcode)
   - App has `get-task-allow` entitlement
   - Can sign code with embedded certificate
   - Works during development

2. **Pre-sign all possible code** (not practical)

3. **Use interpreter for truly dynamic code**
   - Compile to WASM instead of native ARM64
   - Use wasm3 interpreter (like a-Shell)
   - Slower but no signing needed

**Hybrid Approach for jank**:

```
┌─────────────────────────────────────────────────────────────────┐
│                 Hybrid Execution Strategy                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Development (Xcode):                                           │
│    → Compile to ARM64 dylib + ad-hoc sign + dlopen()            │
│    → Full native speed                                          │
│                                                                 │
│  Production (App Store):                                        │
│    → All code AOT compiled                                      │
│    → No runtime code generation                                 │
│                                                                 │
│  Fallback (if signing fails):                                   │
│    → Compile to WASM                                            │
│    → Execute via wasm3 interpreter                              │
│    → Slower but always works                                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Implementation Steps**:

1. Create `IosIncrementalExecutor` class (similar to `WasmIncrementalExecutor`)
2. Use ARM64 backend instead of WebAssembly backend
3. Link with system linker to create .dylib
4. Attempt ad-hoc code signing
5. dlopen() and execute
6. Fall back to WASM interpreter if signing fails

**Pros**:
- Native speed during development
- Works within Apple's rules (when launched from Xcode)
- Fallback path for production
- Leverages existing LLVM infrastructure

**Cons**:
- Complex implementation
- Signing may not work in all scenarios
- Requires both ARM64 and WASM backends

---

### Approach 8: a-Shell Style (Compile to WASM + wasm3 Interpreter)

**Concept**: Follow a-Shell's proven approach - compile to WASM, interpret with wasm3.

**This is a PRODUCTION-PROVEN approach** - a-Shell is on the App Store!

**How a-Shell Does It**:
```bash
# Compile C++ to WebAssembly
clang++ -target wasm32-wasi -o program.wasm program.cpp

# Execute with wasm3 interpreter
wasm3 program.wasm
```

**For jank**:
1. Generate C++ as usual
2. Compile C++ to WASM using Clang's WASM backend
3. Execute via wasm3 (fast WASM interpreter)
4. wasm3 is ~10x faster than reference WASM interpreter

**Pros**:
- **App Store approved** - proven to work
- No JIT or code signing issues
- Works on all iOS devices
- Can hot-reload WASM modules

**Cons**:
- Performance is interpreter-level (~10-100x slower than native)
- Limited WASI capabilities (no sockets, threads)
- Need to bundle wasm3 and WASI SDK

**Performance Reality Check**:

| Operation | Native | wasm3 | Slowdown |
|-----------|--------|-------|----------|
| Simple math | 1x | ~3-5x | Acceptable |
| Function calls | 1x | ~10x | Noticeable |
| Complex loops | 1x | ~20-50x | Significant |

For development/REPL use, this slowdown may be acceptable. Hot reload of logic changes would still feel interactive.

---

## 7. Recommended Implementation Plan

### Phase 1: Simulator JIT (1-2 weeks)

**Goal**: Full JIT development experience on iOS Simulator

**Tasks**:
1. Add `jank_target_ios_simulator` CMake option
2. Modify `ios-bundle` to detect simulator vs device
3. Build full JIT processor for simulator target
4. Test nREPL connection from Emacs/VSCode to simulator
5. Verify hot-reload works

**Expected Result**: `make sdf-ios-sim-run` with live REPL!

### Phase 2: nREPL Hot Reload for Device (2-4 weeks)

**Goal**: Interactive development on physical device

**Tasks**:
1. Implement `wasm-compile-patch` style hot reload for iOS
2. Add cross-compilation to nREPL eval handler
3. Send compiled modules to device
4. Investigate code signing requirements
5. Test with simple code changes

**Expected Result**: Edit jank code, see changes on device (with limitations)

### Phase 3: Optimized Development Workflow (ongoing)

**Goal**: Polish the development experience

**Tasks**:
1. Fast incremental compilation
2. Automatic reconnection
3. Better error reporting
4. Source maps for debugging

---

## 6. Technical Details

### iOS Simulator Build Configuration

```cmake
# In CMakeLists.txt
option(jank_target_ios_simulator "Build for iOS Simulator with JIT support" OFF)

if(jank_target_ios_simulator)
  # iOS Simulator can use JIT!
  set(JANK_JIT_SOURCES
    src/cpp/jank/jit/processor.cpp
    src/cpp/jank/jit/interpreter.cpp
  )
  add_compile_definitions(
    JANK_TARGET_IOS=1
    JANK_TARGET_IOS_SIMULATOR=1
    # Note: NOT defining JANK_TARGET_WASM or JANK_TARGET_EMSCRIPTEN
  )
else()
  # iOS Device - no JIT
  set(JANK_JIT_SOURCES
    src/cpp/jank/jit/processor_stub.cpp
  )
  add_compile_definitions(
    JANK_TARGET_IOS=1
    JANK_TARGET_WASM=1
    JANK_TARGET_EMSCRIPTEN=1
  )
endif()
```

### iOS Simulator Toolchain Considerations

```cmake
# iOS Simulator uses x86_64 or arm64 (for M1+ Macs)
# Same architecture as host Mac
set(CMAKE_OSX_ARCHITECTURES "arm64")  # or x86_64 for Intel
set(CMAKE_OSX_SYSROOT "iphonesimulator")
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0")
```

### nREPL Hot Reload Message Flow

```
Client                     Server (Mac)              Device (iOS)
  │                            │                          │
  │  eval: "(def x 42)"        │                          │
  │ ─────────────────────────► │                          │
  │                            │  compile for iOS         │
  │                            │  cross-compile to .o     │
  │                            │                          │
  │                            │  send compiled module    │
  │                            │ ────────────────────────►│
  │                            │                          │ load module
  │                            │                          │ execute
  │                            │         result           │
  │                            │ ◄────────────────────────│
  │         result             │                          │
  │ ◄───────────────────────── │                          │
```

### Existing nREPL Infrastructure

jank already has `handle_wasm_compile_patch` in the nREPL engine, which:
- Compiles code to WASM format
- Sends patches to browser
- Could be adapted for iOS hot reload

Relevant handler: `compiler+runtime/include/cpp/jank/nrepl_server/ops/wasm_compile_patch.hpp`

---

## 9. Sources

### iOS JIT Restrictions
- [Jailed Just-in-Time Compilation on iOS](https://saagarjha.com/blog/2020/02/23/jailed-just-in-time-compilation-on-ios/) - Saagar Jha's detailed analysis
- [Apple: Allow execution of JIT-compiled code entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit)
- [LuaJIT iOS Issue #1072](https://github.com/LuaJIT/LuaJIT/issues/1072)

### JIT Enabling Tools
- [SideJITServer Guide](https://idevicecentral.com/ios-guide/how-to-enable-jit-on-ios-17-0-18-3-using-sidejitserver/)
- [AltJIT/AltStore](https://faq.altstore.io/altstore-classic/enabling-jit/altjit)
- [iOS Debugging JIT Guides](https://spidy123222.github.io/iOS-Debugging-JIT-Guides/)

### BrowserEngineKit
- [Apple: Using Alternative Browser Engines](https://developer.apple.com/support/alternative-browser-engines/)
- [Creating an Alternative Browser Engine for iOS](https://joel.tools/joelbrowser/)
- [Mozilla BrowserEngineCore JIT Bug](https://bugzilla.mozilla.org/show_bug.cgi?id=1883457)

### Flutter/React Native Patterns
- [Flutter iOS 26 JIT Issue #163984](https://github.com/flutter/flutter/issues/163984)
- [Hermes: JavaScript Engine for React Native](https://engineering.fb.com/2019/07/12/android/hermes/)
- [Bringing Hermes to iOS](https://www.callstack.com/blog/bringing-hermes-to-ios-in-react-native)

### LLVM JIT
- [LLVM ORC Design and Implementation](https://llvm.org/docs/ORCv2.html)
- [JITLink and ORC's ObjectLinkingLayer](https://llvm.org/docs/JITLink.html)
- [Remote JIT Compilation with LLVM](https://weliveindetail.github.io/blog/post/2021/03/29/remote-compile-and-debug.html)

### Clang-Repl and CppInterOp
- [Clang-Repl Official Documentation](https://clang.llvm.org/docs/ClangRepl.html)
- [Cling Transitions to LLVM's Clang-Repl](https://root.cern/blog/cling-in-llvm/)
- [CppInterOp GitHub](https://github.com/compiler-research/CppInterOp)
- [Tutorial Development with Clang-Repl](https://blog.llvm.org/posts/2023-10-5-tutorial-development-with-clang-repl/)

### Clang-Repl in WebAssembly
- [clang-repl-wasm GitHub](https://github.com/anutosh491/clang-repl-wasm) - Debugging xeus-cpp-lite
- [C++ in Jupyter: Interpreting C++ in the Web](https://blog.jupyter.org/c-in-jupyter-interpreting-c-in-the-web-c9d93542f20b)
- [xeus-cpp GitHub](https://github.com/compiler-research/xeus-cpp) - Jupyter kernel for C++
- [LLVM Blog: Compiler Research Internships 2023](https://blog.llvm.org/posts/2023-12-31-compiler-research-internships-2023/)
- [Wasm.cpp Source (WasmIncrementalExecutor)](https://clang.llvm.org/doxygen/Wasm_8cpp_source.html)
- [xeus-cpp-lite Demo](https://compiler-research.org/xeus-cpp/lab/index.html) - Try C++ in browser

### LLVM on iOS
- [LLVM-On-iOS GitHub](https://github.com/light-tech/LLVM-On-iOS) - Script to build LLVM for iOS
- [a-Shell GitHub](https://github.com/holzschu/a-shell) - Terminal for iOS with C/C++ support
- [a-Shell Official Site](https://holzschu.github.io/a-Shell_iOS/)
- [a-Shell on App Store](https://apps.apple.com/us/app/a-shell/id1473805438)
- [WebAssembly for a-Shell Guide](https://bianshen00009.gitbook.io/a-guide-to-a-shell/lets-do-more-for-it/webassembly-for-a-shell)
- [holzschu/iOS_codeSamples](https://github.com/holzschu/iOS_codeSamples) - LLVM on iOS samples

### WebAssembly Runtimes
- [wasm3 GitHub](https://github.com/wasm3/wasm3) - Fast WebAssembly interpreter
- [Running Clang in Browser via Wasmer](https://wasmer.io/posts/clang-in-browser)
- [JIT in WebAssembly (wingolog)](https://wingolog.org/archives/2022/08/18/just-in-time-code-generation-within-webassembly)

### Clojure Mobile Development
- [nREPL](https://nrepl.org/nrepl/index.html)
- [ClojureScript + React Native](https://cljsrn.org/)
- [Replete iOS](https://github.com/replete-repl/replete-ios)

---

## Appendix: Quick Reference

### Current jank iOS Build Defines

```cpp
// Device build (AOT only)
JANK_TARGET_IOS=1
JANK_TARGET_WASM=1
JANK_TARGET_EMSCRIPTEN=1

// Proposed Simulator build (with JIT)
JANK_TARGET_IOS=1
JANK_TARGET_IOS_SIMULATOR=1
// NO WASM/EMSCRIPTEN defines - use full JIT
```

### Key Decision Matrix

| Approach | Complexity | iOS Version | Device | App Store | Recommended |
|----------|------------|-------------|--------|-----------|-------------|
| Simulator JIT | Low | All | Sim only | N/A | ✅ Phase 1 |
| nREPL Hot Reload | Medium | All | Yes | N/A (dev) | ✅ Phase 2 |
| iOS Incremental Executor | High | All | Xcode only | No | ⭐⭐ Best for dev |
| WASM + wasm3 | Medium | All | Yes | **YES** | ⭐ Proven |
| SideJITServer | Low | ≤18.3 | Yes | No | ⚠️ Temporary |
| LLVM Interpreter | High | All | Yes | Yes | ❌ Too slow |
| Bytecode VM | Very High | All | Yes | Yes | ❌ Major rewrite |
| BrowserEngineKit | Medium | 17+ | EU/Japan | No | ❌ Not applicable |

### Recommended Strategy Summary

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     jank iOS Development Strategy                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  IMMEDIATE (Phase 1):                                                       │
│    iOS Simulator with full JIT                                              │
│    → Same experience as macOS development                                   │
│                                                                             │
│  SHORT-TERM (Phase 2):                                                      │
│    nREPL Hot Reload for device development                                  │
│    → Cross-compile on Mac, send to device                                   │
│                                                                             │
│  MEDIUM-TERM (Phase 3):                                                     │
│    iOS Incremental Executor (WasmIncrementalExecutor pattern)               │
│    → Native speed during Xcode development                                  │
│    → Fallback to WASM interpreter                                           │
│                                                                             │
│  LONG-TERM (if needed):                                                     │
│    WASM + wasm3 interpreter for App Store apps                              │
│    → Proven by a-Shell                                                      │
│    → Acceptable for REPL/development                                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```
