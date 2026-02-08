# Native Header Autocompletion

## Overview

This documents how native header autocompletion works in jank's nREPL and how to test it.

## Bug Fix: Type Completion for Native Headers

### The Problem

When completing symbols from native header aliases (like `flecs/`), only functions were returned with proper metadata. Types (structs, classes, enums) were being skipped because `describe_native_header_function` only handled functions.

### The Fix

Added `describe_native_header_type` and `describe_native_header_entity` functions in `engine.hpp`:

1. `describe_native_header_type` - Handles struct/class/enum types from native headers
   - Looks up type using `Cpp::GetScopeFromCompleteName(qualified_name)`
   - Verifies it's a class or enum with `Cpp::IsClass()` / `Cpp::IsEnumScope()`
   - Extracts constructor info for arglists
   - Returns `var_documentation` with `is_cpp_type = true`

2. `describe_native_header_entity` - Tries function first, then falls back to type
   - Used by `complete.hpp`, `eldoc.hpp`, and `info.hpp`
   - Ensures both functions AND types are returned in completions

### Key Changes

- `include/cpp/jank/nrepl_server/engine.hpp` - Added `describe_native_header_type` and `describe_native_header_entity`
- `include/cpp/jank/nrepl_server/ops/complete.hpp` - Changed to use `describe_native_header_entity`
- `include/cpp/jank/nrepl_server/ops/eldoc.hpp` - Changed to use `describe_native_header_entity`
- `include/cpp/jank/nrepl_server/ops/info.hpp` - Changed to use `describe_native_header_entity`

## How It Works

When you require a native C++ header:

```clojure
(ns my-app
  (:require ["test_native.hpp" :as tn]))
```

And type `tn/|` (where `|` is cursor) and press TAB, jank provides autocompletion.

### The Flow

1. **Header registration**: `register-native-header!` in `clojure/core.jank`
   - Derives scope from header path: `"test_native.hpp"` → `"test_native"`
   - Calls `Cpp::ParseAndExecute("#include <test_native.hpp>")`

2. **Scope derivation**: `path->cpp-scope` in `clojure/core.jank`
   - Removes extension: `"flecs.h"` → `"flecs"`
   - Converts `/` to `.`: `"clojure/string_native.hpp"` → `"clojure.string_native"`

3. **Completion query**: `enumerate_native_header_symbols` in `native_header_completion.cpp`
   - Converts scope to C++: `"clojure.string_native"` → `"clojure::string_native"`
   - Uses CppInterOp: `Cpp::GetScopeFromCompleteName()` + `Cpp::GetAllCppNames()`
   - Returns BOTH functions AND types

4. **Describe entities**: `describe_native_header_entity` in `engine.hpp`
   - Tries `describe_native_header_function` first
   - Falls back to `describe_native_header_type` for structs/classes/enums

5. **nREPL integration**: `native-header-functions` in `clojure/core.jank`
   - Called by nREPL completion handler
   - Returns list of symbol names matching prefix

## Tests

### Integration Test (bash)

Created `test/bash/native-header-completion/` with:

- `test_native.hpp` - Simple header with `namespace test_native { ... }`
- `test.jank` - Tests `native-header-functions` API (same as nREPL uses)
- `pass-test` - Babashka test runner

```bash
cd compiler+runtime/test/bash/native-header-completion
./pass-test
```

### Unit Test (C++)

Added test in `test/cpp/jank/nrepl/engine.cpp`:

- `"complete returns native header alias types with proper metadata"`
- Verifies that completions include proper metadata (type, arglists, etc.)

## Key Files

- `include/cpp/jank/nrepl_server/engine.hpp` - `describe_native_header_type`, `describe_native_header_entity`
- `src/jank/clojure/core.jank` - `path->cpp-scope`, `register-native-header!`, `native-header-functions`
- `src/cpp/jank/nrepl_server/native_header_completion.cpp` - `enumerate_native_header_symbols`
- `src/cpp/clojure/core_native.cpp` - `native_header_functions` C++ impl

## Using External Headers

To use external headers like `flecs.h`:

```bash
jank -I/path/to/flecs/distr repl
```

```clojure
(ns my-app
  (:require ["flecs.h" :as flecs]))

;; Completions should work for flecs/ prefix with proper metadata
(flecs/world)  ;; TAB completion shows world, entity, iter, etc.
```

The `-I` flag adds the include path so jank's JIT can find and parse the header.
