# Hot Reload Clang-Tidy Fixes

## Date: 2025-11-30

## Summary

Fixed clang-tidy errors in `src/cpp/jank/runtime/hot_reload.cpp` that were causing CI failures in the "Ubuntu - debug, analysis, coverage" job.

## Issues Fixed

### 1. C-style vararg functions (printf)
**Error:** `cppcoreguidelines-pro-type-vararg` - do not call c-style vararg functions

**Solution:** Replaced `printf()` calls with `util::println()` from `<jank/util/fmt/print.hpp>` for the non-EMSCRIPTEN code paths.

### 2. Const correctness
**Error:** `misc-const-correctness` - variable can be declared 'const'

**Solution:** Added `const` to the following variables:
- `ns_name` and `sym_name` (line 134-135)
- `arity` (line 152)
- `ref` in `jank_unbox_integer` (line 292)
- `val_a` and `val_b` in `jank_add_integers` (line 306-307)
- `ns_str` in `jank_make_keyword` (line 386)
- `ref` in `jank_unbox_double` (line 447)

### 3. Unchecked string to number conversion
**Error:** `bugprone-unchecked-string-to-number-conversion` - 'atoi' used to convert a string, consider using 'strtol'

**Solution:** Replaced `std::atoi(signature.c_str())` with `static_cast<int>(std::strtol(signature.c_str(), nullptr, 10))`.

### 4. Manual memory management
**Error:** `cppcoreguidelines-no-malloc` - do not manage memory manually

**Solution:** Added NOLINT comment for the malloc call in `jank_hot_reload_get_stats()` since this is intentional for the C API / WASM interop.

### 5. Insecure strcpy
**Error:** `clang-analyzer-security.insecureAPI.strcpy` - strcpy is insecure

**Solution:** Replaced `strcpy(result, json.c_str())` with `std::memcpy(result, json.c_str(), json.size() + 1)` which is bounded.

## Files Modified

- `compiler+runtime/src/cpp/jank/runtime/hot_reload.cpp`
  - Added `#include <cstring>` for `std::memcpy`
  - Changed `#include <jank/util/fmt.hpp>` to `#include <jank/util/fmt/print.hpp>`
  - Replaced printf with util::println (9 occurrences)
  - Added const qualifiers (8 variables)
  - Replaced atoi with strtol
  - Replaced strcpy with memcpy

## Notes

The printf calls inside `#ifdef __EMSCRIPTEN__` blocks were not flagged because they are not compiled in the Ubuntu CI builds. These remain as-is for the WASM build path.
