# iOS Remote JIT Compilation Implementation

Date: 2025-12-25

## Overview

This document describes the implementation of remote JIT compilation for iOS. The system enables full JIT compilation on iOS by delegating the heavy compilation work to a macOS server, while iOS handles loading and executing the resulting ARM64 object files.

## Architecture

```
iOS Device                                macOS Compile Server
+-------------------+                     +---------------------+
|                   |                     |                     |
| nREPL Server      |   jank source code  | Compile Server      |
| (port 5558)       | ------------------>| (port 5570)         |
|                   |                     |                     |
| eval_string()     |   ARM64 object file | - Parse & analyze   |
|   |               | <------------------ | - Generate C++      |
|   v               |                     | - Cross-compile     |
| jit_prc.load_object()                   |   to ARM64          |
|   |               |                     |                     |
|   v               |                     +---------------------+
| Execute native    |
| ARM64 code!       |
+-------------------+
```

## Components

### 1. Compile Server (`src/cpp/compile_server_main.cpp`)

The macOS compile server:
- Runs on macOS with full LLVM/Clang toolchain
- Listens on port 5570 (configurable via `--port`)
- Cross-compiles to ARM64 for iOS Simulator or Device
- Uses shared PCH for fast incremental compilation

**Usage:**
```bash
# Build the compile server
ninja -C build compile-server

# Run for iOS Simulator
./build/compile-server --target sim

# Run for iOS Device
./build/compile-server --target device --port 5570
```

### 2. Remote Compile Client (`include/cpp/jank/compile_server/client.hpp`)

iOS client that connects to the compile server:
- Sends jank source code over TCP
- Receives base64-encoded ARM64 object files
- JSON protocol with newline-delimited messages

### 3. Remote Compile Configuration (`include/cpp/jank/compile_server/remote_compile.hpp`)

Global configuration for remote compilation:
- Thread-safe configuration of host/port
- Connect/disconnect functions
- Check if remote compilation is enabled

### 4. C API (`include/cpp/jank/c_api.h`)

```c
// Configure the compile server address
void jank_remote_compile_configure(char const *host, jank_u16 port);

// Connect to the compile server
jank_bool jank_remote_compile_connect(void);

// Disconnect from the compile server
void jank_remote_compile_disconnect(void);

// Check if remote compilation is active
jank_bool jank_remote_compile_is_enabled(void);
```

### 5. Integration in `eval_string` (`src/cpp/jank/runtime/context.cpp`)

When `JANK_IOS_JIT` is defined and remote compilation is enabled:
1. Send code to compile server
2. Receive ARM64 object file
3. Load via `jit_prc.load_object()`
4. Find and call the entry symbol
5. Return the result

## Protocol

### Compile Request
```json
{"op":"compile","id":1,"code":"(defn foo [] 42)","ns":"user","module":""}
```

### Compile Response (Success)
```json
{"op":"compiled","id":1,"symbol":"_user_SLASH_foo_0","object":"<base64>"}
```

### Compile Response (Error)
```json
{"op":"error","id":1,"error":"Syntax error at line 1","type":"compile"}
```

## iOS App Integration

### From Clojure (in nREPL)
```clojure
;; Connect to compile server at startup
(jank.remote-compile/configure "192.168.1.100" 5570)
(jank.remote-compile/connect!)

;; Now eval_string uses remote compilation automatically!
(defn new-frame! [] ...)  ;; Compiled on macOS, executed on iOS
```

### From Objective-C/Swift
```objc
// At app startup
jank_remote_compile_configure("192.168.1.100", 5570);
if (jank_remote_compile_connect()) {
    NSLog(@"Connected to compile server!");
}

// Later, all eval calls use remote compilation
jank_eval_string(...);  // Automatically uses remote compile
```

## Files Changed/Created

- `include/cpp/jank/compile_server/protocol.hpp` - Protocol definitions
- `include/cpp/jank/compile_server/server.hpp` - macOS server
- `include/cpp/jank/compile_server/client.hpp` - iOS client
- `include/cpp/jank/compile_server/remote_compile.hpp` - Config
- `src/cpp/jank/compile_server/remote_compile.cpp` - Config impl
- `src/cpp/compile_server_main.cpp` - Server main
- `include/cpp/jank/c_api.h` - C API additions
- `src/cpp/jank/c_api.cpp` - C API implementation
- `include/cpp/jank/jit/processor.hpp` - Added load_object(data) overload
- `src/cpp/jank/jit/processor.cpp` - Implemented load_object(data)
- `src/cpp/jank/runtime/context.cpp` - Remote compile in eval_string
- `CMakeLists.txt` - Added compile-server target and sources

## Testing

1. Start compile server on macOS:
   ```bash
   ./build/compile-server --target sim
   ```

2. Configure iOS app to connect:
   ```objc
   jank_remote_compile_configure("192.168.1.100", 5570);
   jank_remote_compile_connect();
   ```

3. Run the iOS app and test eval via nREPL

## Next Steps

1. Add nREPL operator for runtime configuration (optional)
2. Add error handling for network failures
3. Add reconnection logic
4. Consider adding caching of compiled objects
