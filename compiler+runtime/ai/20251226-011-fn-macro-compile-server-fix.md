# Fix: fn Macro Not Expanding in Compile Server

## Date: 2025-12-26

## Problem

When using the iOS compile server, expressions using the `fn` macro (including `defn`) would crash with:

```
Assertion failed! set
#2 in jank::codegen::processor::gen(jtl::ref<jank::analyze::expr::var_deref>, ...)
```

Example failing code:
```clojure
(def y (fn [] 42))  ; CRASH!
(defn foo [] 42)    ; CRASH!
```

But simple expressions worked:
```clojure
(def x 1)           ; OK
(+ 100 200 300)     ; OK
```

## Root Cause

The compile server was creating/using namespaces without referring `clojure.core` vars.

When analyzing `(fn [] 42)`:
1. The analyzer looks up `fn` as a symbol
2. It tries to find the var in the current namespace (`user`)
3. `ns::find_var` only looks in the namespace's own vars map
4. Without clojure.core referred, `fn` is not found
5. `macroexpand1` returns the form unchanged (no `:macro` metadata)
6. The analyzer treats `(fn [] 42)` as a FUNCTION CALL instead of a macro
7. This creates a `call` expression instead of a `function` expression
8. During codegen, the `call` expression tries to reference vars that were never lifted
9. `find_lifted_var` returns `none`, and `unwrap()` crashes

## Solution

Added `refer_clojure_core()` helper function in `server.hpp` that:
1. Finds the `clojure.core` namespace
2. Checks if already referred (by checking for `fn` var)
3. If not, iterates all public vars and calls `ns::refer()` for each

Called this function when setting up the namespace for compilation:

```cpp
auto const eval_ns = runtime::__rt_ctx->intern_ns(ns_sym);

// CRITICAL: Refer clojure.core vars to the namespace
// Without this, macros like 'fn' won't be found during macroexpand
refer_clojure_core(eval_ns);
```

## Files Changed

1. **include/cpp/jank/compile_server/server.hpp**
   - Added `refer_clojure_core()` static helper function (lines 124-161)
   - Called it in `compile_code()` after `intern_ns()` (line 334)
   - Added `#include <jank/runtime/obj/keyword.hpp>` for keyword type

## Test Results

Before fix:
```
[compile-server] Compiling code (id=1) in ns=user: (def y (fn [] 42))
Assertion failed! set
```

After fix:
```
[compile-server] Compiling code (id=1) in ns=user: (def y (fn [] 42))
{"op":"compiled","id":1,"symbol":"_user_user_repl_fn_6_7_0",...}
```

## Key Insight

In Clojure, when you switch to a namespace with `(ns foo)`, the macro automatically calls `(refer 'clojure.core)` to make all core vars available. The compile server was using `intern_ns` directly without this crucial step, breaking macro expansion.

## How Clojure Core Knows About `fn`

Looking at `clojure/core.jank`:
```clojure
(defmacro fn
  [& sigs]
  ...)
```

The `fn` macro is defined at line 2987 of core.jank. When clojure.core is loaded:
1. `defmacro` creates a var with `:macro true` metadata
2. When we `refer` this var into our namespace, it becomes available
3. During `macroexpand1`, we check for `:macro` metadata and run the function
4. The macro returns `(fn* ...)` which is the actual special form

Without the refer, the analyzer can't find the `fn` var, can't check its metadata, and treats the form as a regular function call.
