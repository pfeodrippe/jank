# C-Style Variadic Functions in jank

## Summary

jank already supports calling C-style variadic functions (functions using `...` like `printf`, `snprintf`, `ImGui::Text`, etc.) directly without needing `cpp/raw`.

## How It Works

C variadic functions can be called just like any other C++ function. The jank codegen passes all arguments directly to the function call, which the C compiler handles correctly for variadic functions.

## Examples

### Using #cpp Reader Macro (Recommended)

The `#cpp` reader macro provides the cleanest syntax for C++ literals:

```clojure
;; ImGui::Text with format specifiers - clean and simple!
(imgui/Text #cpp "FPS: %d" (rl/GetFPS))

;; Multiple format arguments
(imgui/Text #cpp "%d + %d = %d" 1 2 3)

;; String format
(imgui/Text #cpp "Hello, %s!" #cpp "world")
```

### Using cpp/value (Verbose Alternative)

```clojure
(cpp/raw "#include <cstdio>")

; Single format argument
(let* [n (cpp/int 42)]
  (cpp/printf (cpp/value "\"%d\\n\"") n))

; Multiple format arguments
(let* [a (cpp/int 1)
       b (cpp/int 2)
       c (cpp/int 3)]
  (cpp/printf (cpp/value "\"%d + %d = %d\\n\"") a b c))

; String format
(cpp/printf (cpp/value "\"Hello, %s!\\n\"") (cpp/value "\"world\""))
```

### Calling ImGui::Text
```clojure
;; Clean with #cpp
(imgui/Text #cpp "FPS: %d" (rl/GetFPS))

;; Verbose with cpp/value
(let* [fps (cpp/int (rl/GetFPS))]
  (cpp/ImGui.Text (cpp/value "\"FPS: %d\"") fps))
```

### Custom variadic function
```clojure
(cpp/raw "#include <cstdarg>")
(cpp/raw "int sum(int const n, ...) {
  va_list args;
  va_start(args, n);
  int ret = 0;
  for(int i = 0; i < n; ++i) {
    ret += va_arg(args, int);
  }
  va_end(args);
  return ret;
}")

; Call with varying number of args
(let* [a (cpp/my_ns.sum (cpp/int 0))]             ; sum of 0 args
      [b (cpp/my_ns.sum (cpp/int 2)
                        (cpp/int 10)
                        (cpp/int 20))])           ; sum of 2 args = 30
```

## Key Points

1. **No special syntax needed** - Call C variadic functions like any C++ function
2. **Arguments must be C++ types** - Use `cpp/int`, `cpp/float`, `cpp/value`, etc.
3. **Format strings need escaping** - Use `cpp/value "\"format\\n\""` with escaped quotes

## Test Files

- `test/jank/cpp/call/global/variadic/pass-variadic.jank` - Custom variadic function with va_list
- `test/jank/cpp/call/global/variadic/pass-printf-style.jank` - printf-style calls (NEW)

## Test Results

All 190 tests pass with 2280 assertions.
