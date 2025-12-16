# CI Failures Investigation - nrepl Branch & jank-examples

**Date:** 2025-12-14
**Repos investigated:**
- `pfeodrippe/jank` (nrepl branch) - **NOT PCH timeout** - clang-tidy errors + test failure
- `pfeodrippe/jank-examples` - **YES, PCH timeout confirmed!**

## Summary

### pfeodrippe/jank (nrepl branch)
The failures are NOT caused by PCH generation timeout. Issues are:
1. **clang-tidy errors** (treated as errors via `-Werror`)
2. **1 test failure** in nREPL info tests

### pfeodrippe/jank-examples - PCH TIMEOUT CONFIRMED!
The Linux build IS timing out on PCH generation:
```
[402/407] Generating incremental.pch
2025-12-14T18:49:21Z  <- PCH generation started
2025-12-14T19:08:35Z  <- ninja: build stopped: interrupted by user (TIMEOUT!)
```
**PCH generation ran for 19+ minutes before being killed!**

## Actual CI Failures (from run 20211263985)

### Failure 1: clang-tidy Errors (macOS - debug, analysis)

```
user_type.hpp:14:14: error: enum 'behavior_flag' uses a larger base type ('u32' (aka 'unsigned int'),
  size: 4 bytes) than necessary for its value set, consider using 'std::uint16_t' (2 bytes) as the
  base type to reduce its size [performance-enum-size,-warnings-as-errors]

cpp_util.cpp:711:9: error: repeated branch body in conditional chain [bugprone-branch-clone,-warnings-as-errors]

core_native.cpp:336:14: error: function 'cpp_eval_with_info' can be made static or moved into an
  anonymous namespace to enforce internal linkage [misc-use-internal-linkage,-warnings-as-errors]
```

**Files affected:**
- `include/cpp/jank/runtime/obj/user_type.hpp:14` - `behavior_flag` enum size
- `src/cpp/jank/analyze/cpp_util.cpp:711` - repeated branch body
- `src/cpp/clojure/core_native.cpp:336` - `cpp_eval_with_info` linkage

### Failure 2: Test Failure (Ubuntu - undefined behavior sanitizer)

```
TEST CASE:  info returns proper types for template functions, not auto
  non-template method works to verify header loading

../test/cpp/jank/nrepl/info.cpp:677: FATAL ERROR:
  REQUIRE( arglists_str_it != payload.end() ) is NOT correct!
  values: REQUIRE( {?} != {?} )
```

**Test result:** 228 passed, 1 failed, 0 skipped (out of 229 test cases)

### Build Times (NOT timeout!)

From the CI logs:
- Ubuntu - undefined behavior sanitizer: `Compiled (13m 41s)` - **Build succeeded, tests failed**
- macOS - debug, analysis: Failed at step [2/294] due to clang-tidy errors
- Ubuntu - release: ~12 minutes build time
- macOS - release: ~7 minutes build time

## Fixes Required

### Fix 1: `user_type.hpp` - enum size

```cpp
// Before (line 14):
enum class behavior_flag : u32

// After - use u16 since only 8 flags exist:
enum class behavior_flag : u16
```

### Fix 2: `cpp_util.cpp` - repeated branches

Lines 711-747 have identical branch bodies. Either:
- Combine the cases
- Add `// NOLINT(bugprone-branch-clone)` if intentional

### Fix 3: `core_native.cpp` - internal linkage

```cpp
// Before (line 336):
object_ref cpp_eval_with_info(object_ref const code)

// After:
static object_ref cpp_eval_with_info(object_ref const code)
// Or use anonymous namespace
```

### Fix 4: nREPL info test

The test at `test/cpp/jank/nrepl/info.cpp:677` expects `arglists-str` in payload but it's missing.
Check the `info` op implementation to ensure it returns `arglists-str` for template methods.

## PCH Timeout Analysis (jank-examples)

### Root Cause

The `-fpch-instantiate-templates` flag (introduced in Clang 11) instantiates ALL templates at PCH generation time.
For jank's template-heavy codebase (60+ object types in `visit.hpp`), this takes:
- **macOS (M-series):** ~5-10 minutes (fast single-thread performance)
- **Linux (Intel/AMD):** **19+ minutes** (slower single-thread performance on CI runners)

### Evidence from jank-examples run 20212446444

```
Job: Build jank - Linux
Started: 2025-12-14T18:41:40Z
Completed: 2025-12-14T19:08:37Z (27 min total)

PCH generation:
  Started: 2025-12-14T18:49:21Z ([402/407] Generating incremental.pch)
  Killed:  2025-12-14T19:08:35Z (ninja: build stopped: interrupted by user)
  Duration: ~19 minutes before timeout killed it
```

### Why It's Worse in jank-examples

The `jank-examples` repo builds jank from source without PCH caching. Since there's no cache hit,
the PCH must be regenerated every time, triggering the timeout on Linux.

## Recommended Fix for PCH Timeout

### Option 1: Remove `-fpch-instantiate-templates` for Linux CI (RECOMMENDED)

In `CMakeLists.txt` around line 1224:

```cmake
# Only use -fpch-instantiate-templates on fast machines (macOS M-series)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT DEFINED ENV{CI})
  list(APPEND pch_flags -fpch-instantiate-templates)
endif()
```

**Pros:** Faster PCH generation on Linux CI (potentially 10x speedup)
**Cons:** Slightly slower JIT compilation at runtime

### Option 2: Cache the PCH in jank-examples CI

Add PCH caching alongside the jank build cache:
```yaml
- name: Cache jank build (including PCH)
  uses: actions/cache@v4
  with:
    path: |
      jank/compiler+runtime/build/
    key: jank-build-${{ runner.os }}-${{ hashFiles('jank/compiler+runtime/**/*.hpp', 'jank/compiler+runtime/**/*.cpp') }}
```

**Note:** The current cache only saves the final binary, not the PCH. Including the full build
directory would preserve the PCH between runs.

### Option 3: Increase timeout (not recommended)

```yaml
timeout-minutes: 45  # Increase from current value
```

This just masks the problem and wastes CI resources.

## PCH Technical Details

The PCH command from `CMakeLists.txt:1212-1228`:
```bash
clang++ ... \
  -fpch-instantiate-templates \  # <-- THE SLOW FLAG
  -Xclang -emit-pch \
  -c prelude.hpp \
  -o incremental.pch
```

Headers included in PCH (via `prelude.hpp`):
- `runtime/visit.hpp` - 60+ object type switch (MASSIVE template expansion)
- `runtime/context.hpp` - JIT/analyze processors
- All runtime object types and core functions

## Commands Used for Investigation

```bash
# List recent CI runs for jank repo
gh run list --repo pfeodrippe/jank --branch nrepl --limit 10

# View specific run details with JSON
gh run view 20211263985 --repo pfeodrippe/jank --json jobs

# Get failed logs
gh run view 20211263985 --repo pfeodrippe/jank --log-failed

# Search for specific errors
gh run view 20211263985 --repo pfeodrippe/jank --log-failed | grep -A 5 "FAILED\|error:"

# List jank-examples CI runs
gh run list --repo pfeodrippe/jank-examples --limit 10

# Check jank-examples failed run (PCH timeout)
gh run view 20212446444 --repo pfeodrippe/jank-examples --log-failed | tail -150

# Get job timings
gh run view 20212446444 --repo pfeodrippe/jank-examples --json jobs --jq '.jobs[] | select(.name == "Build jank - Linux")'
```

## Applied Fix (jank-examples / "something" project)

The fix applied to the CI workflow includes:

### 1. Add PCH to cache paths (Linux only)

```yaml
- name: Cache jank build
  uses: actions/cache@v4
  with:
    path: |
      ~/jank/compiler+runtime/build/jank
      ~/jank/compiler+runtime/build/CMakeCache.txt
      ~/jank/compiler+runtime/build/CMakeFiles
      ~/jank/compiler+runtime/build/*.a
      ~/jank/compiler+runtime/build/*.ninja
      ~/jank/compiler+runtime/build/lib
      ~/jank/compiler+runtime/build/incremental.pch  # <-- ADDED
    key: jank-build-linux-x64-${{ env.JANK_REF }}-v7
```

### 2. Branch-based cache key (not commit-based)

Changed from commit-based (`${{ needs.get-jank-commit.outputs.commit }}`) to branch-based (`${{ env.JANK_REF }}`).
This ensures the cache persists across jank commits on the same branch.

### 3. Enable incremental rebuilds

Removed `if: steps.cache-jank.outputs.cache-hit != 'true'` from:
- "Create swap space" step - always create swap (needed if PCH regenerates)
- "Build jank" step - always run ninja to detect changes

```yaml
- name: Create swap space
  run: |
    sudo fallocate -l 8G /swapfile
    sudo chmod 600 /swapfile
    sudo mkswap /swapfile
    sudo swapon /swapfile
    free -h

- name: Build jank
  run: |
    export CC=$HOME/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang
    export CXX=$HOME/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++
    cd ~/jank/compiler+runtime
    ./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Release \
      -Djank_local_clang=on
    ./bin/compile
```

**Result:**
- Cache restores previous build state (including PCH)
- Ninja compares timestamps and rebuilds only changed files
- If headers change → PCH regenerates (with swap available)
- If nothing changed → ninja does nothing (fast no-op)

## Related Documentation

- [Clang PCH Internals](https://clang.llvm.org/docs/PCHInternals.html)
- [LLVM Clang 11 PCH Improvements](https://www.phoronix.com/scan.php?page=news_item&px=LLVM-Clang-11-PCH-Instant-Temp)
- [GitHub Runners Reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
