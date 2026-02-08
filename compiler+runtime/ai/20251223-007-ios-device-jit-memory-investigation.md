# iOS Device JIT Memory Investigation

## Problem
iOS device JIT compilation exceeds memory limit (1850 MB) when compiling `vybe.sdf.ios` module.

## Root Causes Identified

### 1. No Precompiled Header (PCH) on Device
**Impact: ~200-300 MB extra memory usage**

The PCH was disabled for device builds because:
```yaml
# NOTE: PCH is NOT bundled for device builds!
# The PCH built on Mac has Mac filesystem paths embedded, which causes
# redefinition errors when JIT runs on device
```

Without PCH, every JIT compilation must:
- Parse all jank runtime headers fresh
- Parse all native headers (vulkan, imgui, etc.)
- Build AST from scratch

### 2. Heavy Native Header Dependencies
**Impact: ~300-400 MB AST bloat**

The vybe.sdf modules include:
```jank
["vybe/vybe_sdf_math.h" :as _ :scope ""]     ; 281 math functions
["imgui.h" :as imgui :scope "ImGui"]          ; Header-only, template-heavy
["imgui_impl_vulkan.h" :as imgui-vk]          ; Vulkan + ImGui integration
["vulkan/sdf_engine.hpp" :as sdfx]            ; Full Vulkan API
```

### 3. Generated Code Bloat
**Impact: ~100-200 MB during compilation**

Each JIT function generates ~2KB of C++ code with:
- Full struct definition with runtime machinery
- Constructor with var captures
- Type conversion boilerplate

With 100+ functions across vybe.sdf.* modules, this adds up.

### 4. Sequential Module Loading
Each module is JIT compiled independently, but memory isn't fully released between compilations.

## Memory Breakdown During JIT

| Component | Memory |
|-----------|--------|
| LLVM/Clang toolchain | ~300 MB |
| Header AST (no PCH) | ~300-400 MB |
| Generated code AST | ~150-200 MB |
| Codegen buffers | ~100-150 MB |
| Jank runtime | ~100 MB |
| **Total Peak** | **~950-1150 MB per module** |

When loading multiple modules, peaks can exceed 1850 MB.

## Solutions

### Solution 1: Fix PCH for Device Builds (RECOMMENDED)
**Expected savings: ~200-300 MB**

Build PCH specifically for iOS device with bundle-relative paths:
1. Create `build-ios-device-pch` target that builds PCH on device or with device paths
2. Or: Build PCH with placeholder paths and patch at bundle time
3. Or: Build PCH at first app launch and cache it

### Solution 2: AOT Compile Heavy Modules
**Expected savings: Eliminates JIT for those modules**

AOT compile the memory-heavy modules:
- `vybe.sdf.math` (281 functions, 240KB generated)
- `vybe.sdf.ui` (191 functions, 96KB generated)
- `vybe.sdf.shader` (Vulkan code)

Only JIT compile `vybe.sdf.ios` (the entry point with app logic).

### Solution 3: Lazy Module Loading
**Expected savings: Spread memory over time**

Instead of loading all modules at startup:
1. Load only the entry point module
2. Lazy-load other modules on first use
3. Unload compiled code when no longer needed

### Solution 4: Reduce Header Bloat
**Expected savings: ~50-100 MB**

- Move inline functions in vybe_sdf_math.h to a .cpp file
- Use forward declarations where possible
- Split large headers into smaller units

### Solution 5: LLVM Memory Optimization
**Expected savings: ~50-100 MB**

Configure LLVM for lower memory:
```cpp
// In clang_ios.cpp or environment_ios.cpp
args.emplace_back("-Xclang");
args.emplace_back("-frewrite-includes"); // Faster header processing

// Use -O0 to reduce optimization memory
args.emplace_back("-O0");

// Limit template instantiation depth
args.emplace_back("-ftemplate-depth=256");
```

### Solution 6: Incremental JIT
**Expected savings: Significant**

Instead of compiling entire module:
1. Parse module once
2. JIT compile individual functions on-demand
3. Cache compiled functions

This requires significant jank changes.

## Recommended Implementation Order

1. **Immediate**: AOT compile vybe.sdf.math and vybe.sdf.ui (Solution 2)
   - These are the heaviest modules
   - Only JIT the main ios.jank file

2. **Short-term**: Fix PCH for device (Solution 1)
   - Build PCH with relative paths
   - Or build at first launch

3. **Medium-term**: Reduce header bloat (Solution 4)
   - Refactor vybe_sdf_math.h

4. **Long-term**: Lazy loading (Solution 3)

## Quick Test: AOT More Modules

To test Solution 2, add these modules to AOT compilation:

```bash
# In ios-bundle or similar script
./bin/ios-bundle \
  --entry-module vybe.sdf.ios \
  --module-path /path/to/src \
  --output-dir build-iphoneos-jit/aot \
  device
```

This would AOT compile:
- vybe.sdf.math
- vybe.sdf.ui
- vybe.sdf.state
- vybe.sdf.shader
- vybe.sdf.ios

And only require the runtime for calling them, no JIT at all for user code.

## Alternative: Full AOT with Hot Reload

For development iteration without JIT memory issues:
1. Full AOT compile on Mac
2. Push compiled .o files to device via Xcode
3. App detects new .o files and reloads

This gives fast iteration without JIT memory overhead.
