# Docstring Support for Member Methods in Native Header Aliases

## Feature Implemented

Added docstring extraction for member methods when using native header aliases. Previously, only function signatures were returned for member methods - now documentation comments (both C-style and Doxygen) are also extracted.

## Usage

When you query info/eldoc for a member method in a native header alias:

```jank
(require '["flecs.h" :as flecs :scope "flecs"])
```

Then ask for info on `flecs/world.some_method`, the response now includes:
- `doc`: The documentation comment from the C++ source
- `file`: The file where the method is declared
- `line`: The line number (as integer)

## Example Output

For a method with a C-style comment:
```cpp
/* Documented method for testing.
 * This docstring should appear in info/eldoc. */
int documented_method(int x) { return x * 2; }
```

Info returns:
```
doc: "int\n\n    /* Documented method for testing.\n     * This docstring should appear in info/eldoc. */"
file: "./test/cpp/jank/nrepl/test_flecs.hpp"
line: 67
```

For Doxygen-style comments:
```cpp
/// @brief Doxygen-style documented method
/// @param value The input value
/// @return The doubled value
int doxygen_method(int value) { return value * 2; }
```

Info returns:
```
doc: "int\n\n/// @brief Doxygen-style documented method\n    /// @param value The input value\n    /// @return The doubled value"
```

## Implementation Details

### Files Modified

1. **`include/cpp/jank/nrepl_server/engine.hpp`**
   - Added docstring and location extraction to `describe_native_header_function()`
   - Uses existing `extract_cpp_decl_metadata()` function which handles both Doxygen and C-style comments

### Key Code Change

In `describe_native_header_function()`, after building the function signature:

```cpp
/* Extract metadata (docstring, location) from the first function declaration */
auto const metadata(extract_cpp_decl_metadata(fns.front()));
if(metadata.doc.has_value())
{
  info.doc = metadata.doc;
}
if(metadata.file.has_value())
{
  info.file = metadata.file;
}
if(metadata.line.has_value())
{
  info.line = metadata.line;
}
if(metadata.column.has_value())
{
  info.column = metadata.column;
}
```

## Test Results

Added test case "info returns docstring for member methods" which verifies:
- C-style comments are extracted and contain "Documented method"
- Doxygen comments are extracted and contain "@brief"
- File path is extracted correctly
- Line number is present

All 168 tests pass with 2137 assertions.
