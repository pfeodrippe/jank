# Relative Line Numbers in Macro Expansion - Investigation

## Problem

When using `deftest` with nested `is` assertions, line numbers are reported as **relative** to the `deftest` form start rather than **absolute** file positions.

### Example

```clojure
;; In type_test.jank
(deftest get-field-direct-test    ;; Line 252
  (testing "..."
    (let [world ...]
      (is (= 42.8 ...))           ;; Line 259, reported as line 8
      (is (= 99.25 ...))          ;; Line 260, reported as line 9
      ...)))
```

**Observation**:
- Line 8 = 259 - 252 + 1 (relative to deftest, 1-indexed)
- Line 9 = 260 - 252 + 1 (relative to deftest, 1-indexed)

## Investigation Summary

### Components Analyzed

#### 1. Lexer Position Tracking (`src/cpp/jank/read/lex.cpp`)

The `movable_position` struct tracks absolute positions:
- `offset`: byte offset from file start
- `line`: line number (starts at 1)
- `col`: column number (starts at 1)

**Key Code** (lex.cpp:338-354):
```cpp
movable_position &movable_position::operator++() {
  if(proc->file[offset] == '\n') {
    col = 1;
    ++line;  // Increments on newline
  } else {
    ++col;
  }
  ++offset;
  return *this;
}
```

The constructor with offset (lex.cpp:331-336) correctly counts lines:
```cpp
processor::processor(jtl::immutable_string_view const &f, usize const offset)
  : pos{ .proc = this }, file{ f }
{
  pos += offset;  // Uses operator+= which counts newlines
}
```

**Verdict**: Lexer tracks **absolute** positions correctly.

#### 2. Parser Metadata Attachment (`src/cpp/jank/read/parse.cpp`)

Forms get metadata via `source_to_meta()`:
```cpp
// parse.cpp:309-315
return object_source_info{
  make_box<obj::persistent_list>(
    source_to_meta(start_token.start, latest_token.end),
    std::in_place,
    ret.rbegin(),
    ret.rend()),
  start_token,
  latest_token };
```

**Verdict**: Parser attaches positions from lexer tokens directly.

#### 3. Metadata Storage (`src/cpp/jank/runtime/core/meta.cpp`)

The `build_source_meta` function (meta.cpp:23-57) creates metadata:
```cpp
source_map->assoc_in_place(__rt_ctx->intern_keyword("start").expect_ok(), start_map);
source_map->assoc_in_place(__rt_ctx->intern_keyword("end").expect_ok(), end_map);
```

Where `start_map` contains:
- `:offset` - byte offset
- `:line` - line number
- `:col` - column number

**Verdict**: No offset subtraction; stores raw position values.

#### 4. Syntax-Quote Handling (`src/cpp/jank/read/parse.cpp`)

jank intentionally **preserves** metadata through syntax-quote (unlike Clojure which strips it):

```cpp
// parse.cpp:1441-1459
auto const meta{ runtime::meta(form) };
if(meta != jank_nil) {
  /* Note that Clojure removes the source info from the meta here. We're keeping it
   * so that we can provide improved macro expansion errors. */
  auto const quoted_meta{ syntax_quote(meta) };
  // ... wraps with with-meta
}
```

**Unquote-splicing** (`~@`) in `syntax_quote_expand_seq`:
```cpp
// parse.cpp:1141-1143
else if(syntax_quote_is_unquote(item, true)) {
  ret.push_back(second(item));  // Returns spliced form directly
}
```

**Verdict**: Spliced forms retain their original metadata.

#### 5. Macro Expansion (`src/cpp/jank/runtime/context.cpp`)

The `macroexpand` function (context.cpp:861-881):
- Gets metadata from expanded form (NOT the original)
- Adds `:jank/macro-expansion` key pointing to original form

**Verdict**: Expanded forms keep their own metadata.

#### 6. The `is` Macro (`src/jank/clojure/test.jank`)

```clojure
(defmacro is
  ([form msg]
   (let [form-meta (meta form)
         jank-source (get form-meta :jank/source)
         file-val (get jank-source :file)
         line-val (get-in jank-source [:start :line])]
     `(try-expr ~msg ~form ~file-val ~line-val))))
```

The `is` macro extracts line numbers from the `form` argument's metadata at macro expansion time.

### Flow Analysis

1. Reader reads `(deftest my-test (is (= 42 43)))` at line 252
2. Child form `(is (= 42 43))` gets metadata with line 259
3. Child form `(= 42 43)` gets metadata with line 259
4. `deftest` macro expands to `(fn [] ~@body)`
5. Body forms are spliced via `~@body`, preserving metadata
6. When `is` macro runs, `form = (= 42 43)` should have line 259

**But reported line is 8, not 259!**

## Hypotheses

### Hypothesis 1: Reader Line Reset on Nested Forms

When parsing compound forms, the reader might reset line counting relative to the parent form's start position.

**Evidence**: The pattern matches exactly:
- Line 8 = line 259 - line 252 + 1
- Line 9 = line 260 - line 252 + 1

This suggests line numbers are stored as 1-indexed offsets from the parent form.

### Hypothesis 2: File Loading Offset Issue

When loading files, there might be an intermediate step that re-reads portions of the file with a new lexer starting at offset 0, causing line numbers to restart from 1.

**Evidence**: The `reparse_nth` function creates a new lexer at an offset, but this should correctly count lines to that offset.

### Hypothesis 3: Metadata Inheritance/Override

Child forms might be inheriting line numbers from parent forms during some transformation step, causing them to get the parent's relative position instead of absolute.

## Areas Requiring Further Investigation

1. **Top-level form reading**: Check how forms are initially read from files in `context::load_file()` and whether there's any position reset.

2. **`read_string` behavior**: When macros read/eval code, verify that positions are preserved.

3. **Metadata propagation during cons/list creation**: Check if new lists created during macro expansion get metadata from unexpected sources.

4. **Native parser internals**: Look for any position calculation that might subtract a base offset.

## Suggested Debug Approach

Add debugging output in `source_to_meta` to print actual line numbers being stored:

```cpp
// In build_source_meta or source_to_meta
std::cout << "Creating metadata: file=" << source.file
          << " line=" << source.start.line << std::endl;
```

Then run a simple test file to see what line numbers are actually being attached to forms.

## Key Files

| File | Purpose |
|------|---------|
| `src/cpp/jank/read/lex.cpp` | Lexer, position tracking |
| `src/cpp/jank/read/parse.cpp` | Parser, syntax-quote, metadata attachment |
| `src/cpp/jank/runtime/core/meta.cpp` | Metadata creation |
| `src/cpp/jank/runtime/context.cpp` | Macro expansion |
| `src/jank/clojure/test.jank` | `is`, `deftest` macros |

## Related Issues

- This only affects nested forms within macro bodies
- File paths are reported correctly; only line numbers are wrong
- The offset-from-parent pattern is consistent

## Resolution

### Root Cause

The issue was **NOT** in the lexer, parser, or metadata attachment. jank's core infrastructure correctly tracks and stores absolute line numbers.

The problem was in `clojure.test` - the `is` macro and related functions were NOT extracting/passing file/line information from form metadata to `do-report`.

### The Fix

The fix involves updating `src/jank/clojure/test.jank` to:

1. **Update `is` macro** to extract file/line from form's `:jank/source` metadata:
```clojure
(defmacro is
  ([form] `(is ~form nil))
  ([form msg]
   ;; Extract file/line from form metadata once here
   (let [form-meta (meta form)
         jank-source (get form-meta :jank/source)
         file-val (get jank-source :file)
         line-val (get-in jank-source [:start :line])]
     `(try-expr ~msg ~form ~file-val ~line-val))))
```

2. **Update all `assert-expr` methods** to accept 4-arg signature `[msg form file line]`

3. **Update `try-expr`** to accept and pass file/line

4. **Update `assert-predicate` and `assert-any`** to accept and use file/line

5. **Update all `do-report` calls** to include `:file` and `:line` keys

### Files Modified

- `src/jank/clojure/test.jank` - Main fix to pass file/line through the assertion chain

### Verification

Tests confirm line numbers are now correctly reported:
- `test_line_verify.jank` - assertions at lines 24, 26, 28 correctly reported
- `test_ns_require.jank` - loading via `require` also reports correct absolute lines

### Status

**FIX IMPLEMENTED** - The changes are in the working directory but not yet committed. Tests pass (the 10 "failures" are intentional test framework tests that validate failure reporting works).
