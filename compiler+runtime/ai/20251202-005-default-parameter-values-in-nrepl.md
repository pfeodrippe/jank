# Default Parameter Values in nREPL Info/Eldoc

## Summary

C++ functions with default parameter values now show the default values in nREPL info/eldoc responses using the format `{:default VALUE}`.

## Example Output

For a function like:
```cpp
void DrawTextEx(const char *text, int posX = 0, int posY = 0, int fontSize = 20, int color = 0);
```

The nREPL info response shows:
```clojure
[[const char * text] [int posX {:default 0}] [int posY {:default 0}] [int fontSize {:default 20}] [int color {:default 0}]]
```

## Implementation

Added `get_function_arg_default()` helper function in `engine.hpp:2470` that:
1. Gets the `ParmVarDecl` from the Clang AST
2. Uses `hasDefaultArg()` to check if parameter has a default
3. Uses `getDefaultArgRange()` to get the source range
4. Extracts the text from the source using `SourceManager::getBufferData()`

The function extracts the actual default value text from the source code, handling simple literals like numbers and strings.

## Files Modified

- `include/cpp/jank/nrepl_server/engine.hpp:2468-2517` - Added `get_function_arg_default()` helper
- `include/cpp/jank/nrepl_server/engine.hpp:2108-2116` - Uses helper in info response
- `include/cpp/jank/nrepl_server/engine.hpp:2696-2704` - Uses helper in eldoc response
- `test/cpp/jank/nrepl/test_c_header.h:80-84` - Added C++ functions with defaults
- `test/cpp/jank/nrepl/engine.cpp` - Added test case "info shows default parameter values for C++ functions"

## Technical Notes

Initially tried using `clang/AST/Expr.h` and `clang/AST/PrettyPrinter.h` with `printPretty()` to extract default values, but these headers caused Clang segfaults during build. The source range approach using `getDefaultArgRange()` avoids these problematic headers.

## Test Results

```
DrawTextEx arglists: [[const char * text] [int posX {:default 0}] [int posY {:default 0}] [int fontSize {:default 20}] [int color {:default 0}]]
[doctest] test cases: 1 | 1 passed | 0 failed
```
