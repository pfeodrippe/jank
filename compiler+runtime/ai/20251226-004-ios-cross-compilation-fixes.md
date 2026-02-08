# iOS Cross-Compilation Fixes (tinygltf and Flecs)

## Date: 2025-12-26

## Problem 1: tinygltf Duplicate Symbol Error

### Symptom
```
error: In _clojure_core_clojure_core_ns_load_8384_8385_0, duplicate definition of symbol '__ZN8tinygltf6detail6GetKeyE...'
[jank] Error calling -main: Failed to load object for vybe.sdf.screenshot$loading__
```

### Root Cause
The compile server was cross-compiling modules without defining `JANK_IOS_JIT`. This caused `marching_cubes.hpp` to define `TINYGLTF_IMPLEMENTATION` in each cross-compiled module, duplicating the symbols that were already compiled into the iOS app binary.

The condition in `marching_cubes.hpp` was:
```cpp
#if defined(SDF_ENGINE_IMPLEMENTATION) || (!defined(SDF_AOT_BUILD) && !defined(JANK_IOS_JIT))
#define TINYGLTF_IMPLEMENTATION
#endif
```

### Fix
Added `-DJANK_IOS_JIT=1` to the cross-compilation flags in `include/cpp/jank/compile_server/server.hpp`:

```cpp
// Define JANK_IOS_JIT to signal we're cross-compiling for iOS
// This prevents header-only libraries like tinygltf from defining their
// implementation in each module (which would cause duplicate symbol errors)
args.push_back("-DJANK_IOS_JIT=1");
```

## Problem 2: Flecs Symbols Not Found

### Symptom
```
JIT session error: Symbols not found: [ _ecs_struct_init, _ecs_meta_get_bool, _ecs_get_id, ... ]
[jank] Error calling -main: Failed to find symbol _clojure_core_clojure_core_ns_load_9573_9574_0 for vybe.type$loading__
```

### Root Cause
The JIT-only simulator project (`project-jit-only-sim.yml`) didn't include the Flecs source file. When `vybe.type` and `vybe.flecs` modules were cross-compiled, they referenced Flecs functions that weren't linked into the iOS app.

### Fix
Added Flecs source to `SdfViewerMobile/project-jit-only-sim.yml`:

```yaml
# Flecs ECS (needed for vybe.type and vybe.flecs)
- path: ../vendor/flecs/distr/flecs.c
  compilerFlags: ["-O2"]
```

## Files Modified

1. `include/cpp/jank/compile_server/server.hpp` - Added `-DJANK_IOS_JIT=1` flag
2. `/Users/pfeodrippe/dev/something/SdfViewerMobile/project-jit-only-sim.yml` - Added Flecs source

## Testing

Run the compile server and iOS simulator:
```bash
cd /Users/pfeodrippe/dev/something
make sdf-ios-server
# In another terminal:
make ios-jit-only-sim-run
```

All 15+ modules should cross-compile successfully and load on iOS.
