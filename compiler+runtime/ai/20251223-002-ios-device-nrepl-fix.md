# iOS Device nREPL Build Fix - Analysis and Plan

## Problem Analysis

When running `make ios-jit-device-run`, the build fails with:
```
/Users/pfeodrippe/dev/something/SdfViewerMobile/build-iphoneos/generated/jank_aot_init.cpp:21:38: error: expected ';' after top level declarator
/Users/pfeodrippe/dev/something/SdfViewerMobile/build-iphoneos/generated/jank_aot_init.cpp:52:26: error: use of undeclared identifier 'server_server'
```

## Root Cause

The `ios-bundle` script in `/Users/pfeodrippe/dev/jank/compiler+runtime/bin/ios-bundle` has a bug in its module name to C++ identifier conversion.

### The Bug

Lines 659-660 and 684-686 convert module names to C++ function names using:
```bash
func_name="jank_load_$(echo "${module}" | tr '.' '_')"
```

This only replaces `.` with `_`, but **NOT** `-` (hyphens).

### Example

Module: `jank.nrepl-server.server`

Current (broken):
- `extern "C" void* jank_load_jank_nrepl-server_server();` (line 21)
- The hyphen makes this an invalid C++ identifier (`-` is the subtraction operator)
- C++ interprets this as: `jank_load_jank_nrepl` minus `server_server()`

Expected (correct):
- `extern "C" void* jank_load_jank_nrepl_server_server();`
- All dots and hyphens replaced with underscores

### Why Simulator Worked

The simulator build uses JIT mode which doesn't go through the AOT code generation path. It loads modules dynamically at runtime rather than through the generated `jank_aot_init.cpp` file.

## Solution

### Fix 1: Module Name to Function Name Conversion

Changed `tr '.' '_'` to `tr '.-' '__'` in three places:

1. **Line 586** (filename generation):
```bash
# Before
module_filename=$(echo "${module}" | tr '.' '_')

# After
module_filename=$(echo "${module}" | tr '.-' '__')
```

2. **Line 660** (extern declarations):
```bash
# Before
func_name="jank_load_$(echo "${module}" | tr '.' '_')"

# After
func_name="jank_load_$(echo "${module}" | tr '.-' '__')"
```

3. **Line 686** (function calls):
```bash
# Before
func_name="jank_load_$(echo "${module}" | tr '.' '_')"

# After
func_name="jank_load_$(echo "${module}" | tr '.-' '__')"
```

### Why This Works

The jank compiler's AOT code generator already converts hyphens to underscores in the generated C++ code. For example, the generated file contains:
```cpp
extern "C" void* jank_load_jank_nrepl_server_server(){
    return jank::nrepl_server::server::jank_load_jank_nrepl_server_server{ }.call().erase();
}
```

The fix ensures the `jank_aot_init.cpp` uses the same naming convention.

## Files Modified

1. `/Users/pfeodrippe/dev/jank/compiler+runtime/bin/ios-bundle`
   - Lines 586, 660, 686: Changed `tr '.' '_'` to `tr '.-' '__'`

## Additional Context

### Related Fix from Simulator Work

The nREPL server also required a fix to `bootstrap_runtime_once()` in `engine.hpp` to handle hybrid AOT mode:
- Check if clojure.core is already loaded before trying to load it
- Wrap `in_ns` and `refer` calls in try/catch for graceful handling

This fix is already in place and carries over to the device build.

## Testing Plan

1. Run `make ios-jit-device-run`
2. Verify build completes without errors
3. Verify app launches on device
4. Test nREPL connection from host:
   ```bash
   # Connect to device IP on port 5558
   nc -zv <device-ip> 5558
   ```
5. Test code evaluation via nREPL

## Expected Result

After the fix, `jank_aot_init.cpp` should contain:
```cpp
extern "C" void* jank_load_jank_nrepl_server_server();

// ...

extern "C" void jank_aot_init() {
    // ...
    std::cout << "[jank] Loading jank.nrepl-server.server..." << std::endl;
    jank_load_jank_nrepl_server_server();
    // ...
}
```

With valid C++ identifiers that match the function names in the generated module files.
