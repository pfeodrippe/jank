# iOS AOT Build: tinygltf Duplicate Symbol Fix

**Date**: 2025-12-21

## Problem

When building jank modules for iOS AOT that include `sdf_engine.hpp`, linker errors occur due to duplicate symbols from the `tinygltf` header-only library:

```
duplicate symbol 'tinygltf::Skin::operator==(tinygltf::Skin const&) const' in:
    vybe_sdf_screenshot_generated.o
    vybe_sdf_ui_generated.o
    vybe_sdf_iosmain4_generated.o
    sdf_viewer_ios.o
ld: 316 duplicate symbols for architecture arm64
```

## Root Cause

`vulkan/marching_cubes.hpp` (included by `sdf_engine.hpp`) unconditionally defines `TINYGLTF_IMPLEMENTATION` before including `tiny_gltf.h`:

```cpp
// marching_cubes.hpp line 31
#define TINYGLTF_IMPLEMENTATION
#include "tinygltf/tiny_gltf.h"
```

This causes the tinygltf implementation to be compiled into every translation unit that includes `sdf_engine.hpp`.

## Solution

Added `#ifdef` guard around `TINYGLTF_IMPLEMENTATION`:

**vulkan/marching_cubes.hpp** (lines 30-37):
```cpp
// tinygltf for GLB export with vertex colors
// Only define TINYGLTF_IMPLEMENTATION in ONE translation unit (via SDF_ENGINE_IMPLEMENTATION)
#ifdef SDF_ENGINE_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#endif
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tinygltf/tiny_gltf.h"
```

**SdfViewerMobile/sdf_engine_impl.cpp** (new file):
```cpp
// SDF Engine Implementation for iOS
// This file compiles sdf_engine.hpp into a single translation unit
// to avoid duplicate symbols when multiple modules include it

// Define implementation macros BEFORE including the headers
#define SDF_ENGINE_IMPLEMENTATION

#include "../vulkan/sdf_engine.hpp"
```

**SdfViewerMobile/project.yml** - added to sources:
```yaml
- path: sdf_engine_impl.cpp
  compilerFlags: ["-x", "c++"]
```

## Key Insight

The `sdfx::` namespace functions in `sdf_engine.hpp` are defined as `inline`, so they can be included in multiple translation units without duplicate symbol errors. Only the non-inline tinygltf functions needed the implementation guard.

## Related Files Modified

1. `vulkan/marching_cubes.hpp` - Added `#ifdef SDF_ENGINE_IMPLEMENTATION` guard
2. `SdfViewerMobile/sdf_engine_impl.cpp` - New file that defines the implementation
3. `SdfViewerMobile/project.yml` - Added sdf_engine_impl.cpp to sources
4. `SdfViewerMobile/sdf_viewer_ios.mm` - Includes sdf_engine.hpp directly (inline functions need to be visible)

## Build Result

After this fix, iOS AOT modules compile with significantly smaller object sizes:
- `vybe_sdf_iosmain4_generated.o`: 716K → 150K
- `vybe_sdf_ui_generated.o`: 1.0M → 471K

The iOS app builds successfully for iPad Simulator.
