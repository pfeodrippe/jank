# WASM Export Demunging and Stack Size Fixes

## Problem 1: ^:export functions not being exported

When using `^:export` metadata on functions in files with underscores in the filename (e.g., `my_flecs_wasm.jank`), the export wrappers were not being generated.

### Root Cause

In `src/cpp/main.cpp`, the code derived the module name from the file stem:
```cpp
auto const module_name{ file_path.stem().string() };
// my_flecs_wasm.jank -> "my_flecs_wasm"
```

But then it looked up the namespace using this unmunged name:
```cpp
auto const ns(__rt_ctx->find_ns(make_box<obj::symbol>(module_name)));
```

The namespace was actually `my-flecs-wasm` (with hyphens), not `my_flecs_wasm` (with underscores).

### Fix

Added demunging (underscores → hyphens) before namespace lookup in `main.cpp:80-84`:

```cpp
/* Demunge the module name: convert underscores to hyphens for namespace lookup.
 * File names use underscores (my_flecs_wasm.jank) but namespaces use hyphens (my-flecs-wasm). */
std::string ns_name{ module_name };
std::replace(ns_name.begin(), ns_name.end(), '_', '-');
auto const ns(__rt_ctx->find_ns(make_box<obj::symbol>(ns_name)));
```

Also removed the now-redundant `auto const &ns_name(module_name);` line since `ns_name` is now defined earlier.

### Verification

After fix:
```
[jank] Generated WASM export wrapper for: run-flecs
[emscripten-bundle] Found exported function: jank_export_run_flecs
```

The generated C++ correctly uses the hyphenated namespace:
```cpp
extern "C" double jank_export_run_flecs(double arg) {
  auto const var = __rt_ctx->find_var("my-flecs-wasm", "run-flecs");
  // ...
}
```

## Problem 2: Stack overflow when running Flecs

```
Aborted(stack overflow (Attempt to set SP to 0xfffff950, with stack limits [0x00000000 - 0x00010000]))
```

The default WASM stack size is 64KB (0x10000), which is too small for Flecs' deep call stack during `ecs_init`.

### Fix

Added `-sSTACK_SIZE=2097152` (2MB) to `bin/emscripten-bundle` at line 995:

```bash
em_link_cmd+=(
  # Memory settings
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=67108864
  -sSTACK_SIZE=2097152  # 2MB stack for complex libraries like Flecs
  # ...
)
```

## Problem 3: ^:export function signature

Initially `run-flecs` was defined as:
```clojure
(defn ^:export run-flecs [& args]  ; WRONG - variadic
```

But the WASM export wrapper only supports exactly 1 numeric parameter:
```cpp
extern "C" double jank_export_*(double arg) { ... }
```

### Fix

Changed to single parameter:
```clojure
(defn ^:export run-flecs [n]  ; Correct - single param
  ; ... function body ...
  n)  ; Return a number
```

## Commands Used

### Rebuild jank after main.cpp change
```bash
SDKROOT=$(xcrun --show-sdk-path) \
CC=$PWD/build/llvm-install/usr/local/bin/clang \
CXX=$PWD/build/llvm-install/usr/local/bin/clang++ \
ninja -C build jank
```

### Test export generation
```bash
./build/jank run \
  --codegen wasm-aot \
  --module-path src/jank \
  --save-cpp \
  --save-cpp-path /tmp/test.cpp \
  /path/to/my_flecs_wasm.jank
```

Look for: `[jank] Generated WASM export wrapper for: <function-name>`

### Force rebuild WASM (delete cached files)
```bash
rm -f build-wasm/my_flecs_wasm.js build-wasm/my_flecs_wasm.wasm
./bin/emscripten-bundle --skip-build --run \
  --lib /path/to/flecs_wasm.o \
  --lib /path/to/flecs_jank_wrapper.o \
  /path/to/my_flecs_wasm.jank
```

### Test in Node.js
```bash
cd build-wasm && node -e "
import('./my_flecs_wasm.js').then(Module => {
  const mod = Module.default || Module;
  return mod();
}).then(instance => {
  console.log('Calling run-flecs...');
  const result = instance._jank_export_run_flecs(42);
  console.log('Result:', result);
}).catch(e => console.error('Error:', e));
"
```

## Successful Output

```
Calling run-flecs...
=== Flecs WASM Demo === 42
[flecs-wasm] Hello from Flecs WASM wrapper!
[flecs-wasm] Creating Flecs world...
[flecs-wasm] World created!
World created: #object [opaque_box 0x1daf988]
[flecs-wasm] Created entity with id: 578
[flecs-wasm] Created entity with id: 579
Entities: 578 579
Progressed!
[flecs-wasm] Destroying Flecs world...
[flecs-wasm] World destroyed!
=== Done ===
Result: 42
```

## Key Learnings

1. **Namespace convention**: File names use underscores (`my_flecs_wasm.jank`), namespaces use hyphens (`my-flecs-wasm`). The compiler must demunge when looking up namespaces.

2. **WASM stack size**: Default 64KB is insufficient for complex C++ libraries. Use `-sSTACK_SIZE=N` for larger stacks.

3. **^:export limitations**: Currently only supports functions with exactly 1 numeric parameter that return a number. The wrapper uses `double` for JS compatibility.

4. **Debugging exports**: Check for `[jank] Generated WASM export wrapper for:` message and verify `jank_export_*` functions in generated C++.

## Files Modified

- `src/cpp/main.cpp` - Added namespace demunging for ^:export processing
- `bin/emscripten-bundle` - Added `-sSTACK_SIZE=2097152` for 2MB stack
