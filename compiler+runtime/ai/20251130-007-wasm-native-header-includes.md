# WASM AOT: Native Header Includes Support

## Problem

When using `(:require ["flecs.h" :as flecs])` syntax in jank code, the native header worked fine in native mode but failed in WASM mode because:

1. In native mode: The JIT compiler processes the `#include "flecs.h"` directive at runtime
2. In WASM AOT mode: The generated C++ code didn't include the header, causing compilation errors like `use of undeclared identifier 'flecs'`

## Solution

Modified `context.cpp` to emit `#include` directives for native headers in the WASM AOT generated code.

### Changes in `src/cpp/jank/runtime/context.cpp`

After writing the standard jank runtime includes for WASM AOT, the code now:
1. Gets the current namespace using `current_ns()`
2. Gets all native aliases registered in that namespace via `native_aliases_snapshot()`
3. Deduplicates includes (same header may be required with different aliases)
4. Emits `#include` directives for each unique header

```cpp
/* Include native headers from (:require ["header.h" :as alias]) */
auto const curr_ns{ current_ns() };
auto const native_aliases{ curr_ns->native_aliases_snapshot() };
if(!native_aliases.empty())
{
  cpp_out << "\n/* Native headers from :require directives */\n";
  native_set<jtl::immutable_string> seen_includes;
  for(auto const &alias : native_aliases)
  {
    /* Deduplicate includes - same header may be required with different aliases */
    if(seen_includes.insert(alias.include_directive).second)
    {
      cpp_out << "#include " << alias.include_directive.c_str() << "\n";
    }
  }
}
```

## Result

The generated C++ now includes:
```cpp
/* Native headers from :require directives */
#include <flecs.h>
```

This allows WASM builds to compile successfully with native C++ header requirements.

## Testing

Test command:
```bash
./bin/emscripten-bundle --skip-build --run \
    --native-obj /path/to/flecs.o \
    --lib /path/to/flecs_wasm.o \
    -I /path/to/flecs/distr \
    /path/to/my_file.jank
```

## Related Files

- `src/cpp/jank/runtime/context.cpp` - WASM AOT code generation
- `include/cpp/jank/runtime/ns.hpp` - `native_alias` struct and `native_aliases_snapshot()` method
- `src/cpp/clojure/core_native.cpp` - `register_native_header` function
