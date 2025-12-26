# Desktop Mirroring Compile Server Architecture

## Problem Statement

The current standalone compile server approach has fundamental issues:

1. **CppInterOp Configuration Hell**: The compile server needs to parse C++ headers (`vulkan/sdf_engine.hpp`, SDL, ImGui, etc.) to register `native-raw` aliases when evaluating `ns` forms
2. **Duplicated Setup**: We're trying to replicate all the header paths, defines, and CppInterOp configuration that the desktop app already has working
3. **Fragile**: Any change to headers requires updating the compile server configuration

## Insight: Desktop App as Compile Server

The desktop SDF app (`make sdf-clean`) already has everything needed:
- Full jank runtime with CppInterOp properly initialized
- All native headers parsed and working
- The exact same jank code as iOS
- Proven to work!

**Why not embed the compile server directly into the desktop app?**

## Proposed Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Desktop SDF App (macOS)                       │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              Full jank Runtime                            │   │
│  │  - CppInterOp with all headers parsed                    │   │
│  │  - native-raw aliases registered                         │   │
│  │  - Can run app locally for preview                       │   │
│  └──────────────────────────────────────────────────────────┘   │
│                           │                                      │
│                           ▼                                      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │           Embedded Compile Server (new)                   │   │
│  │  - Listens on port 5570                                  │   │
│  │  - Receives jank code from iOS                           │   │
│  │  - Uses EXISTING CppInterOp (headers already parsed!)    │   │
│  │  - Cross-compiles to ARM64 object                        │   │
│  │  - Sends object back to iOS                              │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ TCP (port 5570)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     iOS App (JIT-only)                           │
│                                                                  │
│  - Minimal jank runtime (no header parsing needed)              │
│  - Connects to desktop compile server                           │
│  - Sends jank source code                                       │
│  - Receives ARM64 objects                                       │
│  - Loads and executes via ORC JIT                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Benefits

1. **Zero Configuration**: No need to pass `-I` flags or `JANK_EXTRA_FLAGS` - desktop app already has everything
2. **Single Source of Truth**: Desktop app IS the reference environment for jank code
3. **Live Preview**: You can run the same code on desktop to preview before iOS
4. **Simplified Testing**: Test on desktop first, then push to iOS
5. **Already Works**: `make sdf-clean` proves the environment is correct

## Implementation Options

### Option A: Minimal Cross-Compile Service (Recommended)

The desktop app already does full jank compilation. We just need to:

1. **Add a cross-compile endpoint** that takes generated C++ and produces ARM64 objects
2. **Keep ns evaluation on desktop** (it already works)
3. **Send only the compiled object to iOS**

The cross-compiler is trivial - just invoke clang with iOS target flags.

```cpp
// In desktop app, add a simple TCP server that:
// 1. Receives: {"op": "cross-compile", "cpp": "...", "module": "..."}
// 2. Writes cpp to temp file
// 3. Runs: clang++ -target arm64-apple-ios17.0-simulator -c -o out.o in.cpp
// 4. Returns: {"op": "compiled", "object": "<base64>", "symbol": "..."}
```

### Option B: Full Compile Server in Desktop App

Embed the existing compile server logic into the desktop app:

1. Desktop app starts compile server thread on startup
2. Uses the same `jank::compile_server::server` class
3. But now it has access to the already-initialized runtime context

### Option C: Process Separation with IPC

Keep desktop app and compile server as separate processes, but:

1. Desktop app pre-parses all headers on startup
2. Serializes CppInterOp state or symbol tables
3. Compile server loads the serialized state

(This is complex and probably not worth it)

## Recommendation: Option A - Minimal Cross-Compile Service

The simplest approach:

1. **Desktop app** handles:
   - Full jank compilation (parse, analyze, codegen)
   - Native header parsing (CppInterOp)
   - ns form evaluation
   - Generating C++ code

2. **Cross-compile service** (can be embedded or separate) handles:
   - Taking generated C++ code
   - Running clang with iOS target flags
   - Returning ARM64 object

3. **iOS app** handles:
   - Loading ARM64 objects via ORC JIT
   - Executing the code

### Workflow

```
iOS App                  Desktop App              Cross-Compiler
   │                         │                         │
   │ require(vybe.sdf.ios)   │                         │
   ├────────────────────────►│                         │
   │                         │ 1. Parse ns form        │
   │                         │ 2. Eval ns (registers   │
   │                         │    native aliases)      │
   │                         │ 3. Analyze all forms    │
   │                         │ 4. Generate C++         │
   │                         │                         │
   │                         │ cross-compile(cpp)      │
   │                         ├────────────────────────►│
   │                         │                         │ clang++ -target arm64...
   │                         │◄────────────────────────┤
   │                         │ arm64 object            │
   │◄────────────────────────┤                         │
   │ object + entry symbol   │                         │
   │                         │                         │
   │ load object via ORC     │                         │
   │ execute entry symbol    │                         │
```

## Implementation Plan

### Phase 1: Add Cross-Compile Endpoint to Desktop App

1. Add a simple TCP server to the desktop SDF app (in C++ or as a jank function)
2. Endpoint: `{"op": "cross-compile-ios", "cpp": "...", "module": "..."}`
3. Uses clang to cross-compile to ARM64
4. Returns base64-encoded object

### Phase 2: Modify Desktop App to Handle iOS Require

1. Add endpoint: `{"op": "require-for-ios", "ns": "...", "source": "..."}`
2. Desktop app:
   - Evaluates ns form (registers native aliases)
   - Analyzes all forms
   - Generates C++
   - Cross-compiles to ARM64
   - Returns object

### Phase 3: Modify iOS Client

1. iOS connects to desktop app instead of standalone compile server
2. Sends source code
3. Receives ARM64 objects
4. Loads via ORC JIT

## Alternative: Even Simpler

What if the desktop app just runs jank code and we use **binary serialization** to send the results to iOS?

For functions that don't interact with iOS-specific APIs (like `defn` for pure computations), the desktop could:
1. Eval the code
2. Serialize the result
3. Send to iOS

But this doesn't work for code that needs to call iOS C++ functions.

## Conclusion

The "mirroring" approach makes the most sense:

1. **Desktop app is the development environment** - run code there first
2. **Desktop app is the compile server** - it has all the headers
3. **iOS is a thin execution layer** - just loads and runs objects

This eliminates the complexity of trying to replicate the desktop's CppInterOp setup in a standalone compile server.
