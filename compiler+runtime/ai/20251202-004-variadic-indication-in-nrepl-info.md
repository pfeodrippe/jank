# Fix: Variadic functions now show ... in nREPL info

## Issue

C variadic functions (like `printf`, `ImGui::Text`) were showing only their fixed arguments in nREPL info, with no indication they accept additional arguments:

```
cpp/printf
[[const char * arg0]]
  int
```

This was confusing because users couldn't tell the function is variadic.

## Fix

Added variadic indicator `...` to the function signature display. Now variadic functions show:

```
cpp/printf
[[const char * arg0] ...]
  int
```

## Implementation

Modified `include/cpp/jank/nrepl_server/engine.hpp` at two locations where function signatures are built:

1. `populate_from_cpp_functions` lambda (for global cpp/ functions)
2. `describe_native_header_function` (for header-required functions)

The fix uses Clang AST's `isVariadic()` method via the `get_function_decl()` helper:

```cpp
/* Add variadic indicator if the function takes variable arguments */
auto const *func_decl(get_function_decl(fn));
if(func_decl && func_decl->isVariadic())
{
  if(!first_arg)
  {
    rendered_signature.push_back(' ');
  }
  rendered_signature += "...";
}
```

## Changed Files

- `include/cpp/jank/nrepl_server/engine.hpp`: Added variadic check at lines ~2110 and ~2660
- `test/cpp/jank/nrepl/test_c_header.h`: Added `TraceLog` variadic function for testing
- `test/cpp/jank/nrepl/engine.cpp`: Added two test cases:
  - `info shows variadic indicator for C variadic functions`
  - `eldoc shows variadic indicator for C variadic functions`

## Test Results

All 192 tests pass with 2285 assertions.

### Test Output

```
TraceLog arglists: [[int logLevel] [const char * text] ...]
TraceLog eldoc params count: 3
  param: [int logLevel]
  param: [const char * text]
  param: ...
```
