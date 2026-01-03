# cpp/raw Source Location Line Offset

## Date: 2024-12-04

## Summary

Fixed the line number calculation for functions and variables defined in `cpp/raw` blocks. Previously, the info response returned the line where `cpp/raw` was called, but now it returns the exact line where each declaration is defined within the C++ code.

## Problem

When `cpp/raw` contains multiple declarations on different lines:
```clojure
(cpp/raw "inline void first_fn() { }
inline void second_fn() { }")
```

Previously both functions would report the same line (the `cpp/raw` line). Now each function reports its correct line:
- `first_fn` -> line where `cpp/raw` starts
- `second_fn` -> line + 1

## Solution

CppInterOp adds a 3-line preamble before user code when parsing. The line calculation now subtracts this preamble:

```cpp
constexpr unsigned cpp_interop_preamble_lines{ 3 };

// For each declaration:
auto const cpp_line(source_manager.getSpellingLineNumber(loc));
metadata.origin_line = static_cast<std::int64_t>(src.start.line)
                       + static_cast<std::int64_t>(cpp_line - cpp_interop_preamble_lines - 1);
```

Formula: `jank_line + (clang_line - preamble - 1)`

Where:
- `jank_line` = line where `cpp/raw` is called
- `clang_line` = line number reported by Clang
- `preamble` = 3 (CppInterOp preamble lines)
- `-1` = because line 1 of user code is on the same line as `cpp/raw`

## Files Modified

### 1. `src/cpp/jank/evaluate.cpp`
- Added `cpp_interop_preamble_lines` constant (value 3)
- Updated FunctionDecl metadata line calculation
- Updated VarDecl metadata line calculation

### 2. `test/cpp/jank/nrepl/engine.cpp`
- Updated test case "info returns jank source location for cpp/raw functions"
- Now tests two functions on different lines to verify offset calculation

## Test Case

```cpp
TEST_CASE("info returns jank source location for cpp/raw functions")
{
  // cpp/raw on line 4 with two functions:
  // - first_fn on C++ line 1 -> jank line 4
  // - second_fn on C++ line 2 -> jank line 5

  std::string const file_contents =
    "; Comment line 1\n"
    "; Comment line 2\n"
    "\n"
    "(cpp/raw \"inline void test_first_fn(int x) { }\n"
    "inline void test_second_fn(int y) { }\")\n";

  // Test verifies: first_fn at line 4, second_fn at line 5
}
```
