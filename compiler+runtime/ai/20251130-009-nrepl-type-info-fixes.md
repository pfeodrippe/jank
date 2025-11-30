# nREPL Type Info and Location Fixes

## Problem

When querying info for native header functions like `flecs.entity.child`, the nREPL server was returning:
1. "NULL TYPE" for function argument and return types
2. Relative file paths instead of full absolute paths

Example output before fix:
```
cpp/flecs.entity.child
[[NULL TYPE r] [NULL TYPE args]]
[[NULL TYPE args]]
  flecs::entity
```

## Root Cause

### NULL TYPE Issue
The `Cpp::GetTypeAsString()` function from CppInterOp returns "NULL TYPE" when it cannot properly stringify certain complex types (especially template types or types it doesn't fully understand). The code was not checking for this edge case.

### Relative Path Issue
The `extract_cpp_decl_metadata()` function was using `src_mgr.getPresumedLoc(loc).getFilename()` which returns the filename as it appeared in the source (often a relative path), rather than the resolved absolute path.

## Solution

### 1. NULL TYPE Fix

In `engine.hpp`, added checks for "NULL TYPE" in the type string and fallback to alternative methods:

```cpp
auto const arg_type(Cpp::GetFunctionArgType(fn, idx));
if(arg_type)
{
  auto const type_str = Cpp::GetTypeAsString(arg_type);
  // Check for "NULL TYPE" which indicates CppInterOp couldn't stringify the type
  if(type_str.find("NULL TYPE") == std::string::npos)
  {
    arg_doc.type = type_str;
  }
  else
  {
    // Try to get a better type representation using qualified name
    auto const type_scope = Cpp::GetScopeFromType(arg_type);
    arg_doc.type = type_scope ? Cpp::GetQualifiedName(type_scope) : "auto";
  }
}
else
{
  arg_doc.type = "auto";
}
```

This pattern was applied to:
- `describe_native_header_function()` - for both return types and argument types
- `describe_cpp_function()` (the `populate_from_cpp_functions` lambda)
- `describe_native_header_type()` - for constructor argument types

### 2. Full Path Fix

In `extract_cpp_decl_metadata()`, changed the file path extraction to use the file entry's real path:

```cpp
// Try to get the full path from the file entry
auto const file_id = src_mgr.getFileID(loc);
if(auto const file_entry = src_mgr.getFileEntryRefForID(file_id))
{
  auto const real_path = file_entry->getFileEntry().tryGetRealPathName();
  if(!real_path.empty())
  {
    result.file = real_path.str();
  }
  else
  {
    // Fall back to the filename from the file entry
    result.file = file_entry->getName().str();
  }
}
else
{
  // Fall back to the presumed filename
  result.file = std::string(filename);
}
```

## Files Changed

- `include/cpp/jank/nrepl_server/engine.hpp`:
  - `extract_cpp_decl_metadata()` - lines ~1550-1572 (file path extraction)
  - `describe_cpp_function()` (lambda `populate_from_cpp_functions`) - lines ~1960-2035 (NULL TYPE checks)
  - `describe_native_header_function()` - lines ~2357-2436 (NULL TYPE checks)
  - `describe_native_header_type()` - lines ~2604-2642 (NULL TYPE checks for constructors)

## Testing

Run nREPL engine tests:
```bash
./jank-test --test-suite="nREPL engine"
```

The fix makes arglists display proper types like:
- `[[this_param_test::world this]]`
- `[[this_param_test::world this] [int x] [float y]]`

Instead of `[[NULL TYPE this] [NULL TYPE x]]`.

## Key APIs Used

- `Cpp::GetTypeAsString(type)` - converts type to string (may return "NULL TYPE")
- `Cpp::GetScopeFromType(type)` - gets the scope/declaration from a type
- `Cpp::GetQualifiedName(scope)` - gets fully qualified name of a scope
- `src_mgr.getFileEntryRefForID(file_id)` - gets file entry from source location
- `file_entry->getFileEntry().tryGetRealPathName()` - gets resolved absolute path
