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

### 1b. Template Parameter Types (Clang AST Fallback)

For template parameter types like `Args&&...`, even `Cpp::GetScopeFromType()` returns null because template parameters don't have a concrete scope. The solution is to use Clang's AST directly.

**Important**: CppInterOp's `fn` pointer can be either a `clang::FunctionDecl*` or a `clang::FunctionTemplateDecl*` depending on whether the function is a template. We need to handle both cases:

```cpp
/* Get FunctionDecl from a CppInterOp function pointer.
 * Handles both FunctionDecl and FunctionTemplateDecl (extracts templated decl). */
clang::FunctionDecl const *get_function_decl(void *fn) const
{
  if(!fn)
  {
    return nullptr;
  }
  auto const *decl = static_cast<clang::Decl const *>(fn);
  /* Try FunctionDecl first */
  if(auto const *func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl))
  {
    return func_decl;
  }
  /* For function templates, get the templated FunctionDecl */
  if(auto const *tmpl_decl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl))
  {
    return tmpl_decl->getTemplatedDecl();
  }
  return nullptr;
}

/* Get argument type as string using Clang AST directly.
 * This handles template parameter types that CppInterOp fails to stringify. */
std::string get_function_arg_type_string(void *fn, size_t idx) const
{
  auto const *func_decl = get_function_decl(fn);
  if(!func_decl || idx >= func_decl->getNumParams())
  {
    return "auto";
  }
  auto const *param = func_decl->getParamDecl(idx);
  if(!param)
  {
    return "auto";
  }
  auto type_str = param->getType().getAsString();
  if(type_str.empty() || type_str.find("NULL TYPE") != std::string::npos)
  {
    return "auto";
  }
  return type_str;
}

/* Get return type as string using Clang AST directly. */
std::string get_function_return_type_string(void *fn) const
{
  auto const *func_decl = get_function_decl(fn);
  if(!func_decl)
  {
    return "auto";
  }
  auto type_str = func_decl->getReturnType().getAsString();
  if(type_str.empty() || type_str.find("NULL TYPE") != std::string::npos)
  {
    return "auto";
  }
  return type_str;
}
```

The key insight is that CppInterOp's `fn` pointer is actually a `clang::Decl*` that can be either a `FunctionDecl` or `FunctionTemplateDecl`. For templates, we use `getTemplatedDecl()` to get the underlying `FunctionDecl` which has the actual parameter type information.

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

### CppInterOp APIs
- `Cpp::GetTypeAsString(type)` - converts type to string (may return "NULL TYPE")
- `Cpp::GetScopeFromType(type)` - gets the scope/declaration from a type
- `Cpp::GetQualifiedName(scope)` - gets fully qualified name of a scope

### Clang AST APIs
- `llvm::dyn_cast<clang::FunctionDecl>(decl)` - safe cast to FunctionDecl
- `llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)` - safe cast to FunctionTemplateDecl
- `tmpl_decl->getTemplatedDecl()` - get the FunctionDecl from a FunctionTemplateDecl
- `func_decl->getParamDecl(idx)` - get parameter declaration at index
- `func_decl->getReturnType()` - get return type
- `param->getType().getAsString()` - get parameter type as string (handles template params)
- `src_mgr.getFileEntryRefForID(file_id)` - gets file entry from source location
- `file_entry->getFileEntry().tryGetRealPathName()` - gets resolved absolute path

### Required Headers
```cpp
#include <clang/AST/DeclBase.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>  // For FunctionTemplateDecl
#include <llvm/Support/Casting.h>    // For llvm::dyn_cast
```

## Known Issue: PCH Build Crash

The incremental PCH build may crash with a Clang/Boost compatibility issue:
```
/opt/homebrew/include/boost/lexical_cast/detail/converter_lexical.hpp:216:9: current parser token 'template'
clang++: error: clang frontend command failed with exit code 139
```

This is unrelated to the type info fixes and is a known issue with certain Clang/Boost combinations on macOS. The code changes are syntactically correct and compile successfully when tested in isolation.

## Test Results

All 55 nREPL engine tests pass with 475 assertions.

Template type tests show proper type extraction:
- Non-template method: `[[template_type_test::entity this]]`
- Variadic template: `[[Args &&... args]]` (not "auto")
- Simple template: `[[T value]]` (not "auto")
- Mixed parameters: `[[const char * name] [T && value]]`
- Return types: `entity` (not "auto")

## Test Files Added

- `test/cpp/jank/nrepl/template_types.hpp` - Test header with template functions
- Test case in `test/cpp/jank/nrepl/engine.cpp`: "info returns proper types for template functions, not auto"
  - Subcases: non-template method, variadic template, simple template with T, mixed parameters

### Test Header Pattern

To test with custom headers:
1. First include via `cpp/raw`:
   ```clojure
   (cpp/raw "#include \"test/cpp/jank/nrepl/template_types.hpp\"")
   ```
2. Then require with `:scope`:
   ```clojure
   (require '["test/cpp/jank/nrepl/template_types.hpp" :as tmpl-test :scope "template_type_test"])
   ```

This two-step process is needed because:
- `cpp/raw` compiles the header into the JIT
- `require` with `:scope` creates the namespace alias for the scope's types and functions
