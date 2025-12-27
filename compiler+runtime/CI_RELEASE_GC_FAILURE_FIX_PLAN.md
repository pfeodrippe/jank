# CI Release Build GC Failure Fix Plan

## Problem Description

The Release job in CI is failing with a garbage collector memory exhaustion error:

```
Too many retries in GC_alloc_large
Aborted (core dumped)
```

**Location**: Build step [260/288] during core library compilation
**Command**: `jank-phase-1 compile-module` compiling clojure.core, jank.nrepl-server.core, jank.nrepl-server.server, and jank.export
**Build Configuration**: Release build with `-O2 -DNDEBUG` optimization flags
**File**: compiler+runtime/CMakeLists.txt:1123

## Root Cause Analysis

1. **Memory Exhaustion**: The bdwgc (Boehm-Demers-Weiss Garbage Collector) runs out of memory when allocating large objects during compilation
2. **Release Optimization Impact**: The `-O2` optimization level produces larger intermediate compilation objects
3. **Core Module Complexity**: The core modules (especially clojure.core) are large and complex, requiring significant memory during compilation
4. **CI Resource Constraints**: CI environments typically have limited memory compared to development machines

## Solution Approaches

### Option 1: Increase GC Heap Size (Recommended)

**Implementation**:
Set GC environment variables in the CI workflow before the Release build:

```yaml
- name: Configure GC for Release Build
  run: |
    echo "GC_INITIAL_HEAP_SIZE=512M" >> $GITHUB_ENV
    echo "GC_MAXIMUM_HEAP_SIZE=2G" >> $GITHUB_ENV
```

**Pros**:
- Simple, non-invasive fix
- No code changes required
- Directly addresses the memory exhaustion issue
- Can be tuned based on CI runner capabilities

**Cons**:
- Requires understanding of CI runner memory limits
- May need adjustment over time as codebase grows

**Implementation Steps**:
1. Locate the CI workflow file (likely `.github/workflows/*.yml`)
2. Add environment variable configuration before the Release build step
3. Test with conservative values first (512M initial, 2G max)
4. Monitor and adjust if needed

---

### Option 2: Split Core Library Compilation

**Implementation**:
Modify CMakeLists.txt to compile core modules in separate build targets instead of a single command.

**Pros**:
- Reduces peak memory usage per compilation unit
- More granular build process
- Better for incremental builds

**Cons**:
- Requires significant CMake refactoring
- More complex build configuration
- Longer total build time

**Implementation Steps**:
1. Identify the compilation command at CMakeLists.txt:1123
2. Split into separate `add_custom_command` targets for each module
3. Create proper dependencies between targets
4. Test build process

---

### Option 3: Reduce Optimization for Core Compilation

**Implementation**:
Use `-O1` or `-Og` specifically for the jank-phase-1 compilation step in Release builds:

```cmake
set(CMAKE_CXX_FLAGS_RELEASE_PHASE1 "-O1 -DNDEBUG")
```

**Pros**:
- Reduces memory pressure during compilation
- Still maintains optimized final output

**Cons**:
- Longer compilation time for core modules
- May produce slightly less optimal code
- Adds complexity to build configuration

**Implementation Steps**:
1. Add conditional compilation flags in CMakeLists.txt
2. Apply reduced optimization only to jank-phase-1 step
3. Keep `-O2` for final binary compilation

---

### Option 4: Add Explicit GC Collection Points

**Implementation**:
Add `GC_gcollect()` calls in jank-phase-1 compiler between module compilations.

**Pros**:
- More controlled memory management
- Proactive approach to prevent exhaustion

**Cons**:
- Requires code changes in compiler
- May increase compilation time
- Need to identify optimal collection points

**Implementation Steps**:
1. Locate jank-phase-1 compilation loop in source code
2. Add `GC_gcollect()` after each module compilation
3. Test impact on compilation time and memory usage

---

## Recommended Solution

**Option 1: Increase GC Heap Size** is the recommended approach because:

1. **Simplicity**: Single configuration change, no code modifications
2. **Effectiveness**: Directly addresses the root cause (memory exhaustion)
3. **Low Risk**: Easy to revert or adjust
4. **Maintainability**: Easier to understand and document
5. **Quick Implementation**: Can be deployed immediately

### Implementation Plan

1. **Identify CI Workflow File**
   - Search for `.github/workflows/*.yml` files
   - Locate the Release build job

2. **Add GC Configuration**
   ```yaml
   - name: Build Release
     env:
       GC_INITIAL_HEAP_SIZE: 512M
       GC_MAXIMUM_HEAP_SIZE: 2G
     run: |
       cmake --build build --config Release
   ```

3. **Test and Monitor**
   - Run CI build and verify success
   - Monitor memory usage in CI logs
   - Adjust values if needed (increase to 3G-4G if still failing)

4. **Document**
   - Add comment in workflow file explaining the configuration
   - Update CI documentation with memory requirements

### Fallback Plan

If Option 1 doesn't fully resolve the issue:
1. Combine with Option 3 (reduce optimization to `-O1`) as a temporary measure
2. Investigate Option 2 (split compilation) for long-term scalability
3. Profile memory usage to identify specific memory-intensive operations

## Additional Considerations

- **CI Runner Specs**: Verify available memory on CI runners (typically 7GB for GitHub Actions standard runners)
- **Long-term Scalability**: As the codebase grows, may need to revisit compilation strategy
- **Local Development**: Developers with limited RAM may encounter similar issues
- **Memory Profiling**: Consider adding memory usage metrics to CI logs for monitoring
