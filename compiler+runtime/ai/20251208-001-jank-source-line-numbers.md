# Fix for :jank/source Metadata Line Numbers

## Problem

When CIDER (or other nREPL clients) evaluates a single form (like a `deftest`) extracted from a buffer, the `:jank/source` metadata on forms inside that code had **relative** line numbers instead of **absolute** file line numbers.

For example, if a `deftest` is at line 252 in a file and contains an `is` assertion at line 260, the `:jank/source` metadata would report line 8 (260-252) instead of line 260.

## Root Cause

1. When nREPL receives an eval request, it extracts `line` and `column` hints from the message (sent by CIDER)
2. The `handle_eval` function calls `eval_string(code)` without passing these hints
3. `eval_string` creates a lexer with `read::lex::processor l_prc{ code };`
4. The lexer's default constructor initializes position at line 1, column 1
5. All forms parsed from this code get line numbers relative to 1, not to the actual file position

## Solution

Three changes were made:

### 1. New Lexer Constructor (lex.hpp/lex.cpp)

Added a constructor that accepts initial line/column:

```cpp
processor(jtl::immutable_string_view const &f, usize start_line, usize start_col);
```

This initializes the position directly with the provided line/column values instead of defaults.

### 2. New eval_string Overload (context.hpp/context.cpp)

Added an overload that accepts starting line/column:

```cpp
object_ref eval_string(jtl::immutable_string_view const &code, usize start_line, usize start_col);
```

The original `eval_string(code)` now calls `eval_string(code, 1, 1)` for backward compatibility.

### 3. Updated handle_eval (eval.hpp)

Modified `handle_eval` to use the new overload when line/column hints are available:

```cpp
auto const result(line_hint.has_value() && column_hint.has_value()
                    ? __rt_ctx->eval_string(code_view, line_hint.value(), column_hint.value())
                    : __rt_ctx->eval_string(code_view));
```

## Files Changed

- `include/cpp/jank/read/lex.hpp` - Added new constructor declaration
- `src/cpp/jank/read/lex.cpp` - Added new constructor implementation
- `include/cpp/jank/runtime/context.hpp` - Added new eval_string overload declaration
- `src/cpp/jank/runtime/context.cpp` - Added new eval_string overload implementation
- `include/cpp/jank/nrepl_server/ops/eval.hpp` - Use line/column hints in handle_eval

## Testing

All existing tests pass (218/219 test cases, 2458/2459 assertions - same as baseline).
