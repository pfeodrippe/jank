# iOS Compile Server Namespace Resolution Fixes

## Date: 2025-12-26

## Problem
iOS apps with bundle paths containing `jank` in the path (e.g., `.../app.src.jank.vybe.sdf.ios`) caused namespace resolution errors during cross-compilation. The generated C++ code had unqualified `jank::runtime` references that incorrectly resolved to the iOS bundle path namespace instead of the global `::jank::runtime`.

## Root Causes

### 1. Native Header Include Format
**File:** `compiler+runtime/include/cpp/jank/compile_server/server.hpp`

The native header includes were generated without angle brackets:
```cpp
// Before
cpp_code += "#include " + std::string(alias.header.data(), alias.header.size()) + "\n";

// After
cpp_code += "#include <" + std::string(alias.header.data(), alias.header.size()) + ">\n";
```

### 2. Missing `::` Prefix in Entry Functions
**File:** `compiler+runtime/include/cpp/jank/compile_server/server.hpp`

The generated entry functions used unqualified `jank::runtime`:
```cpp
// Before
cpp_code += "extern \"C\" jank::runtime::object_ref " + entry_symbol + "() {\n";
cpp_code += "  return jank::runtime::make_box<" + qualified_struct + ">()->call();\n";

// After
cpp_code += "extern \"C\" ::jank::runtime::object_ref " + entry_symbol + "() {\n";
cpp_code += "  return ::jank::runtime::make_box<" + qualified_struct + ">()->call();\n";
```

### 3. Missing User Include Paths
**File:** `compiler+runtime/include/cpp/jank/compile_server/server.hpp`

The cross-compiler didn't have access to user project include paths (e.g., for `vulkan/sdf_engine.hpp`):
```cpp
// Added in create_compile_server_config()
for(auto const &inc : util::cli::opts.include_dirs)
{
  config.include_paths.push_back(inc);
  std::cout << "[compile-server] Added user include path: " << inc << std::endl;
}
```

### 4. Unqualified `object_ref` in Codegen
**File:** `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

Several places used `object_ref` without the `::jank::runtime::` prefix:
```cpp
// Before (line 1118, 1125, 1434)
util::format_to(body_buffer, "{{ object_ref {}(jank_nil); ", munged_name);
util::format_to(body_buffer, "{ object_ref {}({}); ", munged_name, ...);
util::format_to(body_buffer, "object_ref {}{ };", ret_tmp);

// After
util::format_to(body_buffer, "{{ ::jank::runtime::object_ref {}(::jank::runtime::jank_nil); ", munged_name);
util::format_to(body_buffer, "{ ::jank::runtime::object_ref {}({}); ", munged_name, ...);
util::format_to(body_buffer, "::jank::runtime::object_ref {}{ };", ret_tmp);
```

### 5. Unqualified Type Name in cpp_util
**File:** `compiler+runtime/src/cpp/jank/analyze/cpp_util.cpp`

The `get_qualified_type_name` function returned unqualified type names:
```cpp
// Before (line 316)
return "jank::runtime::object_ref";

// After
return "::jank::runtime::object_ref";
```

## Why These Fixes Were Needed

When the iOS bundle path contains `jank` (e.g., `.../app.src.jank.vybe.sdf.ios`), the generated namespace becomes:
```cpp
namespace Users::...::app::src::jank::vybe::sdf::ios { ... }
```

Inside this namespace, an unqualified `jank::runtime` reference tries to resolve to:
```cpp
Users::...::app::src::jank::runtime  // WRONG - doesn't exist
```

Instead of:
```cpp
::jank::runtime  // CORRECT - global namespace
```

The `using namespace ::jank::runtime;` directive doesn't help because C++ lookup rules check the local namespace first.

### 6. Missing Includes in server.hpp
**File:** `compiler+runtime/include/cpp/jank/compile_server/server.hpp`

When building with `-Djank_test=on`, the compile server failed to build due to missing includes for runtime functions used in server.hpp:
```cpp
// Added these includes:
#include <jank/runtime/core/to_string.hpp>   // for runtime::to_string
#include <jank/runtime/core/seq.hpp>         // for runtime::sequence_length
#include <jank/runtime/rtti.hpp>             // for runtime::try_object
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_list.hpp>
#include <jank/runtime/obj/symbol.hpp>
```

## Testing
1. Start desktop app: `make sdf-ios-server`
2. Run iOS simulator: `make ios-jit-only-sim-run`
3. Verify log shows: `[compile-server] Clang exited with status: 0`
4. Run tests: `./bin/test` - should have only 1 failure (pre-existing nREPL info test)
