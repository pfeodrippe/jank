# iOS Simulator JIT Fix - COMPLETED

## Summary
Fixed iOS Simulator JIT build to match the device JIT flow exactly. The simulator was missing:
1. `project-jit-sim.yml` xcodegen spec (was using stale manually-created project)
2. `ios-jit-sim-project` Makefile target to regenerate the project
3. Proper library linking (`-lvybe_aot` and library search paths)

## Analysis Summary

### Why Device JIT Works but Simulator JIT Doesn't

**Device JIT Flow** (working):
1. `ios-jit-device-aot` → calls `build_ios_jank_aot.sh device` → generates:
   - `build-iphoneos/libvybe_aot.a` (contains ALL modules including user modules)
   - `build-iphoneos/generated/jank_aot_init.cpp`

2. `ios-jit-device-libs` → copies:
   - JIT libs from `build-ios-device-jit/` (libjank.a, etc.)
   - `jank_aot_init.cpp` from `build-iphoneos/generated/`

3. `ios-jit-device-project` → regenerates Xcode project via xcodegen

4. **Xcode project (`project-jit-device.yml`)** links:
   ```yaml
   LIBRARY_SEARCH_PATHS:
     - $(PROJECT_DIR)/build-iphoneos-jit
     - $(PROJECT_DIR)/build-iphoneos    # ← HAS THIS
   OTHER_LDFLAGS:
     - "-lvybe_aot"                      # ← HAS THIS (all AOT modules)
   ```

**Simulator JIT Flow** (broken):
1. `ios-jit-sim-aot` → calls `build_ios_jank_aot.sh simulator` → generates:
   - `build-iphonesimulator/libvybe_aot.a` (contains ALL modules)
   - `build-iphonesimulator/generated/jank_aot_init.cpp`

2. `ios-jit-sim-libs` → copies:
   - JIT libs from `build-ios-sim-jit/`
   - `jank_aot_init.cpp` from `build-iphonesimulator/generated/`

3. **NO `ios-jit-sim-project` target!** Uses stale `SdfViewerMobile-JIT.xcodeproj`

4. **Xcode project (`project-jit.yml`)** MISSING:
   ```yaml
   LIBRARY_SEARCH_PATHS:
     - $(PROJECT_DIR)/build-iphonesimulator-jit
     # MISSING: build-iphonesimulator
   OTHER_LDFLAGS:
     # MISSING: "-lvybe_aot"
   ```

### The Linker Error Explained

The `jank_aot_init.cpp` from the full AOT build references:
```cpp
extern "C" void* jank_load_vybe_sdf_math();  // etc.
```

But the simulator JIT Xcode project DOESN'T link `libvybe_aot.a` (which contains these symbols).

## Fix Steps

### 1. Create `project-jit-sim.yml`
Copy from `project-jit-device.yml` and modify:
- Change name to `SdfViewerMobile-JIT`
- Change `build-iphoneos-jit` → `build-iphonesimulator-jit`
- Change `build-iphoneos` → `build-iphonesimulator`
- Update LLVM paths for simulator (`ios-llvm-simulator`)
- Keep iPad simulator SDK configuration

### 2. Add `ios-jit-sim-project` Makefile target
```makefile
.PHONY: ios-jit-sim-project
ios-jit-sim-project: ios-jit-sim-libs
	@echo "Generating simulator JIT Xcode project..."
	cd SdfViewerMobile && xcodegen generate --spec project-jit-sim.yml
	@echo "Project generated!"
```

### 3. Update `ios-jit-sim-run` dependency chain
```makefile
ios-jit-sim-run: ios-jit-sync-sources ios-jit-sync-includes ios-jit-sim-project
```

### Key Points
- The "JIT" mode is actually hybrid: AOT core libs + JIT for updates
- Both device and simulator JIT link `libvybe_aot.a` (full AOT)
- The JIT capability is for runtime code reload, not initial loading
- Simulator must match device flow exactly (just different target architecture)

## Files Changed

1. **Created `SdfViewerMobile/project-jit-sim.yml`**
   - Copied from `project-jit-device.yml` with simulator-specific paths
   - Uses `build-iphonesimulator-jit` and `build-iphonesimulator`
   - Uses `ios-llvm-simulator` for LLVM headers

2. **Updated `Makefile`**
   - Modified `ios-jit-sim-libs` to copy folly and create merged LLVM library
   - Added `ios-jit-sim-project` target to regenerate Xcode project
   - Updated `ios-jit-sim-run` to depend on `ios-jit-sim-project`

## Result
Both device and simulator JIT builds now follow identical flows:
```
ios-jit-{device,sim}-run
  → ios-jit-sync-sources
  → ios-jit-sync-includes
  → ios-jit-{device,sim}-project
      → ios-jit-{device,sim}-libs
          → ios-jit-{device,sim}-aot (calls build_ios_jank_aot.sh)
      → xcodegen generate --spec project-jit-{device,sim}.yml
  → xcodebuild
  → simctl/devicectl install & launch
```
