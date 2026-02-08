# nREPL Test Op - Fix actual/expected Format and Line Numbers

## Problem

When using the nREPL `test` operation with CIDER, failing test results had several issues:

1. **actual/expected showed forms instead of values:**
   - Bad: `"actual" "(not (= 41 42))"`, `"expected" "(= 41 (vybe.flecs/eid w 42))"`
   - Good: `"actual" "42\n"`, `"expected" "41\n"`

2. **context field showed 0 instead of nil:**
   - Bad: `"context" 0`
   - Good: `"context" nil`

3. **Missing file and line fields that CIDER expects**

4. **Test failures didn't show correct source file and line numbers**

## Solution

### Part 1: nREPL custom assert-expr (test.hpp)

1. **`include/cpp/jank/nrepl_server/ops/test.hpp`**:
   - Added custom `assert-expr` implementation for `=` that captures actual/expected values directly
   - **Fixed `context` field**: Changed from `bencode::value{}` to string `"nil"`
   - **Added `file` and `line` fields**: Extract from test report metadata

2. **`test/cpp/jank/nrepl/engine.cpp`**:
   - Added test case verifying the fix

### Part 2: clojure.test file/line extraction (test.jank)

**Important Performance Note:** We originally tried adding top-level `:line`, `:column`, `:file` keys to `build_source_meta` in `meta.cpp`, but this caused a **3x performance regression** (60fps → 20fps). The `build_source_meta` function is called for every form read, so creating 4 map entries instead of 1 for every form was too expensive.

**Solution:** Keep metadata structure unchanged (`:jank/source` nested only), and extract file/line **once** in the `is` macro, then pass through function chain. This is efficient because extraction only happens at macro expansion time, not on every form read.

**Key Changes:**

1. **`is` macro** extracts file/line from form metadata and passes to `try-expr`:
```clojure
(defmacro is
  ([form] `(is ~form nil))
  ([form msg]
   (let [form-meta (meta form)
         jank-source (get form-meta :jank/source)
         file-val (get jank-source :file)
         line-val (get-in jank-source [:start :line])]
     `(try-expr ~msg ~form ~file-val ~line-val))))
```

2. **`try-expr` macro** takes 4 args `[msg form file line]` and passes to `assert-expr`

3. **`assert-expr` multimethod** dispatch function changed from 2 to 4 args:
```clojure
(defmulti assert-expr
  (fn [msg form file line]
    (cond
      (nil? form) :always-fail
      (seq? form) (first form)
      :else :default)))
```

4. **All assert-expr methods** updated to take 4 args `[msg form file line]`:
- `:always-fail`
- `:default`
- `'instance?`
- `'thrown?`
- `'thrown-with-msg?`
- `'=` (custom in test.hpp)
- `'clojure.core/=` (custom in test.hpp)

5. **Helper functions** updated:
- `assert-predicate` - takes `[msg form file line]`
- `assert-any` - takes `[msg form file line]`

## How It Works

When you write a test in a `.jank` file:
```clojure
(deftest my-test
  (is (= 42 43)))  ; Line 5
```

1. **Reader** attaches metadata to forms: `{:jank/source {:file "test.jank" :start {:line 5 ...} ...}}`
2. **`is` macro** extracts file/line ONCE from `(get-in (meta form) [:jank/source ...])`
3. **`is` macro** expands to `(try-expr msg form file line)`, passing file/line as args
4. **`try-expr`** passes args to `(assert-expr msg form file line)`
5. **`assert-expr` methods** include file/line in `do-report` calls
6. **`report :fail`** calls `testing-vars-str` which displays file and line

Result:
```
FAIL in (my-test) (test.jank:5)
expected: (= 42 43)
  actual: (not (= 42 43))
```

## Bencode nil handling

Bencode doesn't have a native nil type. The `bencode::value{}` default constructor creates an integer `0`. For CIDER compatibility, we use the string `"nil"` which CIDER interprets correctly.

## Test Results

- Before: 217 passed, 1 failed
- After: 219 passed, 1 failed (2 new test cases added)

The 1 failure is expected from the intentional `my-failing-test` test case.

## Note on nREPL-evaluated code

Tests evaluated from strings via nREPL (like `eval_string("(deftest foo ...)")`) will still show `NO_SOURCE_PATH:1` because:
1. `*file*` is `"NO_SOURCE_PATH"` when not loading from a file
2. Strings don't have meaningful file/line metadata

This only affects tests defined via nREPL eval. Tests loaded from actual `.jank` files work correctly.

## Known Issue: Relative Line Numbers in deftest

**Bug:** Line numbers for test assertions inside `deftest` are reported as *relative* to the deftest form start, not as absolute file line numbers.

For example, if `deftest` starts at line 252 and an `is` assertion is at line 260:
- Expected: line 260 (absolute)
- Actual: line 8 (relative: 260 - 252)

The file path is reported correctly; only the line number is wrong.

**Root Cause:** This appears to be a jank compiler issue with how metadata line numbers are preserved through macro expansion. Forms inside syntax-quoted macro bodies (like `~@body` in `deftest`) get line numbers relative to the enclosing form rather than absolute file positions.

**Impact:** CIDER will navigate to the wrong line when clicking on failing tests, though the file will be correct.

**Fix Required:** This needs to be addressed in jank's reader/analyzer to preserve absolute line numbers through macro expansion. Tracked separately from the nREPL test op implementation.

## References

- [CIDER-nrepl test extensions](https://github.com/clojure-emacs/cider-nrepl/blob/master/src/cider/nrepl/middleware/test/extensions.clj)
- [clojure.test API](https://clojure.github.io/clojure/clojure.test-api.html)
