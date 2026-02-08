# C Header Macros and Global Function Fixes

## Summary

This document covers:
1. Fix for global C function calls with empty scope
2. Removal of debug output from native_header_index
3. Investigation into C macro support (like raylib's RAYWHITE)

## Fix: Global C Function Calls

### The Problem

When calling global C functions from raylib.h with `:scope ""`, users got:
```
error: Member function calls need an invoking object.
```

For example:
```clojure
(ns my-raylib
  (:require ["raylib.h" :as rl :scope ""]))

(rl/GetMouseX)  ;; ERROR: Member function calls need an invoking object
```

### Root Cause

In `processor.cpp`, the `rewrite_native_symbol` lambda was unconditionally adding a `.` before the member name:

```cpp
native_transient_string scoped(alias_info.scope.data(), alias_info.scope.size());
if(!member_name.empty())
{
  if(member_name[0] != '.')
  {
    scoped.push_back('.');  // BUG: Always adds dot!
  }
  scoped.append(member_name.data(), member_name.size());
}
```

This caused `rl/GetMouseX` with scope `""` to become `cpp/.GetMouseX` (note leading dot), which is interpreted as a member access requiring an invoking object.

### The Fix

Only add the dot separator if there's a scope prefix:

```cpp
if(!scoped.empty() && member_name[0] != '.')
{
  scoped.push_back('.');
}
```

Now `rl/GetMouseX` with empty scope becomes `cpp/GetMouseX` (no leading dot), which is correctly interpreted as a global function call.

### File Modified

- `src/cpp/jank/analyze/processor.cpp` (around line 1551)

## Cleanup: Debug Output Removal

Removed verbose debug logging from `native_header_index.cpp` that was printing to stderr on every completion request:

```
native_header_index::list_functions header='raylib.h' scope='' prefix='FA'
native_header_index::list_functions cache entries=597
native_header_index::list_functions first entries:
  - AttachAudioMixedProcessor
  - AttachAudioStreamProcessor
  ...
```

### File Modified

- `src/cpp/jank/nrepl_server/native_header_index.cpp`
  - Removed `util::println` calls from `list_functions()` and `contains()`
  - Removed unused `#include <jank/util/fmt/print.hpp>`

## Implementation: C Macro Support

### The Request

Support C macros like raylib's color constants:
```c
#define RAYWHITE   CLITERAL(Color){ 245, 245, 245, 255 }
```

So users could write:
```clojure
(ClearBackground rl/RAYWHITE)
```

### What Was Implemented

Added three new functions to `native_header_completion.cpp` for macro support:

1. **`enumerate_native_header_macros(alias, prefix)`**
   - Iterates all macros in Clang's Preprocessor
   - Filters by header file (only macros defined in the specified header)
   - Filters by prefix for completion
   - Only returns object-like macros (not function-like)

2. **`is_native_header_macro(alias, name)`**
   - Checks if a name is a defined macro from the specified header
   - Returns false for function-like macros

3. **`get_native_header_macro_expansion(alias, name)`**
   - Returns the token string expansion of a macro
   - For example, `TEST_PI` returns `"3.14159f"`
   - For compound literals like `TEST_WHITE`, returns `"CLITERAL ( Color ) { 255 , 255 , 255 , 255 }"`

### How Macros Work in Clang

Macros are processed by the preprocessor before the AST is built. However, Clang's Preprocessor keeps macro definitions in its table after header inclusion.

### Access to Preprocessor

CppInterOp provides access via `Cpp::GetInterpreter()`:

```cpp
#include <Interpreter/Compatibility.h>
#include <clang/Lex/Preprocessor.h>

auto* interp = static_cast<compat::Interpreter*>(Cpp::GetInterpreter());
auto& CI = *interp->getCI();
auto& PP = CI.getPreprocessor();

// Check if macro is defined
bool defined = PP.isMacroDefined("RAYWHITE");

// Get macro info
auto* II = PP.getIdentifierInfo("RAYWHITE");
auto* MI = PP.getMacroInfo(II);
```

### Evaluation Strategies

#### 1. Simple Constant Macros (`#define PI 3.14159`)

Use `Cpp::Evaluate()` directly:
```cpp
intptr_t val = Cpp::Evaluate("PI");
```

#### 2. Compound Literal Macros (like RAYWHITE)

RAYWHITE expands to `(Color){ 245, 245, 245, 255 }` - a compound literal creating a struct.

Strategy: Generate code to capture the macro value:
```cpp
// Generate unique variable name
std::string var_name = "_jank_macro_RAYWHITE_" + unique_id();

// Declare variable with macro value
Cpp::Declare(("Color " + var_name + " = RAYWHITE;").c_str());

// Get the variable's address
auto* var = Cpp::GetNamed(var_name.c_str());
intptr_t addr = Cpp::GetVariableOffset(var);

// Now we have a pointer to the Color struct
```

### Implementation Plan

1. **Check if macro exists**
   - When `rl/RAYWHITE` isn't found as function/type/variable
   - Check `PP.isMacroDefined("RAYWHITE")`

2. **Enumerate macros from header** (for completion)
   - Iterate `PP.macros()`
   - Filter by source file (like we do for functions)
   - Only include macros defined in the specified header

3. **Evaluate macro at use site**
   - Generate C++ code to capture macro value
   - Use `Cpp::Declare()` to create a variable
   - Access the variable's value/address

4. **Type inference**
   - For struct macros, we need to determine the result type
   - Could parse the macro tokens or rely on the declared variable's type

### Challenges

1. **Type determination**: How do we know RAYWHITE is a `Color`?
   - Option A: Require explicit type annotation `:macros {RAYWHITE Color}`
   - Option B: Parse macro tokens to find the type
   - Option C: Use `auto` and let Clang infer

2. **AOT compilation**: Macros are JIT-only unless we generate code at compile time

3. **Function-like macros**: `#define MAX(a,b) ((a)>(b)?(a):(b))`
   - More complex to handle
   - Would need to generate wrapper functions

### Implemented Syntax: `alias/MACRO_NAME`

Macros from native headers can now be accessed using the standard alias syntax:

```clojure
(ns my-raylib
  (:require ["raylib.h" :as rl :scope ""]))

;; Access macros via alias/MACRO_NAME - works for simple constant macros
(let [key-escape rl/KEY_ESCAPE    ; Integer constant macro
      pi rl/TEST_PI]              ; Float constant macro
  (println "KEY_ESCAPE =" key-escape)
  (println "PI =" pi))
```

The implementation transforms `rl/KEY_ESCAPE` into `(cpp/value "KEY_ESCAPE")` when the symbol is detected as a macro from the header. This works for object-like macros (not function-like macros).

**Note:** For compound literal macros like raylib's `RAYWHITE`, which expand to struct initializers, the evaluation happens at JIT time. These macros return the computed value as a native pointer that can be passed to C functions.

### Future/Alternative Syntax

For more explicit control, these syntaxes could be added in the future:

```clojure
;; Option 1: Explicit macro list
(ns my-raylib
  (:require ["raylib.h" :as rl :scope ""
             :macros [RAYWHITE RED GREEN BLUE]]))

;; Option 2: All macros (could be slow)
(ns my-raylib
  (:require ["raylib.h" :as rl :scope "" :include-macros true]))

;; Option 3: Macro type hints
(ns my-raylib
  (:require ["raylib.h" :as rl :scope ""
             :macros {RAYWHITE Color, RED Color, KEY_ESCAPE int}]))
```

### Related CppInterOp APIs

```cpp
// Execute code
int Cpp::Declare(const char* code, bool silent = false);
int Cpp::Process(const char* code);
intptr_t Cpp::Evaluate(const char* code, bool* HadError = nullptr);

// Get interpreter
TInterp_t Cpp::GetInterpreter();

// Symbol lookup
TCppScope_t Cpp::GetNamed(const std::string& name, TCppScope_t parent = nullptr);
intptr_t Cpp::GetVariableOffset(TCppScope_t var, TCppScope_t parent = nullptr);
TCppType_t Cpp::GetVariableType(TCppScope_t var);
```

## References

- [raylib.h on GitHub](https://github.com/raysan5/raylib/blob/master/src/raylib.h)
- [CLITERAL compound literal issue](https://github.com/raysan5/raylib/issues/1343)
- CppInterOp header: `third-party/cppinterop/include/clang/Interpreter/CppInterOp.h`
- Clang Preprocessor: `clang/Lex/Preprocessor.h`

## Macro Autocompletion

Macros from native headers are now included in TAB completion. When you type `rl/` and press TAB, you'll see both functions and macros from the header.

This was implemented by adding macros to `enumerate_native_header_symbols()` in `native_header_completion.cpp`.

## Macro Documentation

When requesting info on a macro (e.g., via nREPL's `info` op), the macro expansion is shown as the docstring:

```
#define KEY_ESCAPE 256
#define TEST_WHITE CLITERAL ( Color ) { 255 , 255 , 255 , 255 }
```

This is implemented via `describe_native_header_macro()` in `engine.hpp`.

## Callable Macro Syntax

Macros can be used in call position with no arguments:

```clojure
;; Both work and return the macro value
rl/KEY_ESCAPE      ;; => 256
(rl/KEY_ESCAPE)    ;; => 256
```

This is useful for consistency and to support future function-like macros. The implementation in `analyze_call()` detects when a macro symbol (which produces a `cpp_call` expression) is in call position with no arguments and just returns the evaluated value instead of trying to call it.

## Files Modified

### Macro Implementation
- `include/cpp/jank/nrepl_server/native_header_completion.hpp`
  - Added function declarations for macro support
- `src/cpp/jank/nrepl_server/native_header_completion.cpp`
  - Added Preprocessor includes
  - Implemented `enumerate_native_header_macros()`
  - Implemented `is_native_header_macro()`
  - Implemented `get_native_header_macro_expansion()`
- `src/cpp/jank/analyze/processor.cpp`
  - Added macro detection in `rewrite_native_symbol` lambda
  - When `alias/MACRO_NAME` is used and MACRO_NAME is a macro from the header,
    transforms it to `(cpp/value "MACRO_NAME")` and analyzes that
  - Added check in `analyze_call` at lines 3176-3182 and 3281-3287 to handle callable macros:
    When a `cpp_call` expression (from macro evaluation) is in call position with no arguments,
    just return the expression instead of trying to call it

### Macro Autocompletion & Docstring
- `src/cpp/jank/nrepl_server/native_header_completion.cpp`
  - Added macros to `enumerate_native_header_symbols()` (lines 595-607)
- `include/cpp/jank/nrepl_server/engine.hpp`
  - Added `is_cpp_macro` field to `var_documentation` struct
  - Added `describe_native_header_macro()` function for macro documentation
  - Modified `describe_native_header_entity()` to also check for macros
- `include/cpp/jank/nrepl_server/ops/info.hpp`
  - Added handling for `is_cpp_macro` type in the type determination logic

### Test Files
- `test/cpp/jank/nrepl/test_c_header.h`
  - Added test macros: `TEST_WHITE`, `TEST_BLACK`, `TEST_RED`, `TEST_GREEN`, `TEST_BLUE`
  - Added simple constant macros: `TEST_PI`, `TEST_MAX_VALUE`, `KEY_ESCAPE`
  - Added `Color` struct and `CLITERAL` macro (mimicking raylib)
- `test/cpp/jank/nrepl/engine.cpp`
  - Added 4 new test cases for macro functions
  - Added test case for `alias/MACRO` syntax via nREPL
  - Added test case for callable macro syntax `(th/KEY_ESCAPE)`
- `test/jank/cpp/macro/pass-simple-constant.jank`
  - End-to-end test verifying `cpp/value` can evaluate C macros
- `test/jank/cpp/macro/pass-alias-syntax.jank`
  - End-to-end test verifying macros can be evaluated

## Testing

**IMPORTANT:** The C++ macro tests must be run from the `build/` directory because they use relative paths like `../test/...`.

```bash
# Run all tests from the build directory
cd compiler+runtime/build

# Run global C function tests
./jank-test --test-case="*global*"

# Run macro tests (all 7 tests)
./jank-test --test-case="*macro*"

# Run macro end-to-end tests
JANK_JIT_TEST_FILTER="macro/pass" ./jank-test --test-case="files"
```

All tests should pass:
- 6 global C function tests
- 7 macro tests (enumeration, filtering, checking, expansion, alias syntax)
