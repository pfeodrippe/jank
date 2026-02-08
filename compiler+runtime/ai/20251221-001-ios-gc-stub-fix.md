# iOS Build: GC_throw_bad_alloc Fix

## Issue
When linking the iOS build, the linker reported:
```
Undefined symbols for architecture arm64:
  "GC_throw_bad_alloc()", referenced from:
      gc::operator new(unsigned long, GCPlacement) in vybe_sdf_generated.o
```

## Root Cause
The `gc_wasm_stub.cpp` file provides a stub implementation of `GC_throw_bad_alloc()` for WASM builds because the BDWGC WASM build doesn't include `gc_badalc.cc`.

The iOS build has the same situation - it doesn't build `gc_badalc.cc` either, so it needs the same stub.

## Fix
Added `gc_wasm_stub.cpp` to the iOS build sources in CMakeLists.txt:

```cmake
elseif(jank_target_ios)
  # iOS build: runtime without JIT/LLVM (similar to WASM)
  list(APPEND jank_lib_sources ${jank_runtime_common_sources})
  list(APPEND jank_lib_sources
    # ... other sources ...
    # GC stub for missing gc_badalc.cc (same as WASM)
    src/cpp/jank/gc_wasm_stub.cpp
  )
```

## Other Fixes in This Session

### Duplicate Symbol Error
The project.yml had both:
1. A source file entry for `generated/vybe_sdf_generated.cpp` (compiled by Xcode)
2. A linker flag for `$(PROJECT_DIR)/build/obj/vybe_sdf_generated.o` (pre-compiled)

Fixed by removing the source file entry since we use pre-compiled objects for consistency with the clojure core libraries.

### Library Search Paths
Updated project.yml to use simulator build directory:
- `/Users/pfeodrippe/dev/jank/compiler+runtime/build-ios-simulator`
- `/Users/pfeodrippe/dev/jank/compiler+runtime/build-ios-simulator/third-party/bdwgc`

## Build Commands
```bash
# Rebuild jank iOS runtime for simulator
./bin/build-ios build-ios-simulator Release simulator

# Regenerate Xcode project
xcodegen generate --spec SdfViewerMobile/project.yml

# Build and run
xcodebuild -project SdfViewerMobile.xcodeproj -scheme SdfViewerMobile \
  -sdk iphonesimulator -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPad Pro 13-inch (M4)' build

xcrun simctl launch 'iPad Pro 13-inch (M4)' com.vybe.SdfViewerMobile
```
