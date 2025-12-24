# iOS Device nREPL - Complete Solution

## Summary
Successfully got iOS device JIT with nREPL working on iPad. The app loads AOT-compiled modules and runs an nREPL server on port 5558 for remote eval.

## Issues Fixed (in order)

### 1. Module Loading Order
- **Problem**: `jank.nrepl-server.asio` loaded before `clojure.core` → stack overflow
- **Fix**: Modified `ios-bundle` to insert nREPL asio loading after core libs in `jank_aot_init()`

### 2. Conditional Compilation
- **Problem**: `#if defined(JANK_IOS_JIT)` guard in `jank_aot_init.cpp` never compiled because `libvybe_aot.a` built without that flag
- **Fix**: Removed conditional, always load nREPL asio unconditionally

### 3. UIKit Main Thread
- **Problem**: Running `-main` on pthread caused UIKit crash ("Call must be made on main thread")
- **Fix**: Run `-main` on main thread (only module loading needs large stack pthread)

### 4. Exception Handling
- **Problem**: `jtl::immutable_string` exceptions not caught, caused silent crash
- **Fix**: Added catch blocks for all jank exception types in `call_jank_main_impl()`

### 5. Thread Bindings
- **Problem**: nREPL IO thread didn't have `*ns*` bindings → "Cannot set non-thread-bound var"
- **Fix**: Added `binding_scope` to nREPL server's IO thread in `asio.cpp`

## Architecture

```
Main Thread (iOS ~1MB stack):
├── init_jank_runtime_on_large_stack() → spawns pthread with 8MB stack
│   └── [8MB pthread] Load all AOT modules (needs deep stack for JIT codegen)
├── call_jank_main() → runs on main thread (UIKit requirement)
│   └── vybe.sdf.ios/-main
│       ├── Start nREPL server on 0.0.0.0:5558
│       │   └── [IO thread with binding_scope]
│       └── Run render loop
```

## Key Files Modified
1. `compiler+runtime/bin/ios-bundle` - Module loading order
2. `compiler+runtime/src/cpp/jank/nrepl_server/asio.cpp` - Thread bindings
3. `SdfViewerMobile/sdf_viewer_ios.mm` - Exception handling, UIKit threading

## Connect from Mac
```bash
# In Emacs/CIDER or any nREPL client
lein repl :connect 192.168.x.x:5558  # iPad's IP address
```
