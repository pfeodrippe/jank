# nREPL pprint/print Middleware Fixes

Date: 2025-12-11

## Issues Found

### 1. Duplicate `#'` prefix for vars in pprint

**Problem:** When pretty-printing vars, the output showed `#'#'namespace/name` instead of `#'namespace/name`.

**Cause:** In `clojure/pprint.jank`, line 149 had:
```clojure
(var? x) (str "#'" x)
```

Since `var.to_string()` in jank already returns `#'namespace/name` (see `var.cpp:60`), adding another `#'` prefix caused duplication.

**Fix:** Changed to `(var? x) (str x)` to use the var's built-in string representation.

### 2. Missing `cider.nrepl.pprint/pr` function

**Problem:** CIDER's print middleware was requesting `cider.nrepl.pprint/pr`, but only `pprint` was defined.

**Fix:** Added `pr` function to `cider/nrepl/pprint.jank`:
```clojure
(defn pr
  "Print value using pr (non-pretty-printed)"
  [value writer]
  (clojure.core/pr value))
```

### 3. Missing emit_pending_output after pprint handling

**Problem:** Output captured during `require` (for the pprint namespace) was never emitted because `emit_pending_output()` wasn't called after the pprint handling block.

**Fix:** Added `emit_pending_output()` call in `eval.hpp` before adding the value message to responses.

## Files Changed

1. `compiler+runtime/src/jank/clojure/pprint.jank` - Fixed var string representation
2. `compiler+runtime/src/jank/cider/nrepl/pprint.jank` - Added `pr` function
3. `compiler+runtime/include/cpp/jank/nrepl_server/ops/eval.hpp` - Added emit_pending_output call

## Related Code Paths

- Print middleware handling: `eval.hpp:255-316`
- Var to_string: `var.cpp:58-61`
- Output redirect: `core.cpp:33-41`
- pprint-str: `pprint.jank:133-150`
