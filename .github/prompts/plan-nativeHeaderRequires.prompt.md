## Plan: Native Header Requires

Allow `ns` forms to bind C++ headers directly so symbols like `str-native/reverse` compile without manual `cpp/raw` includes. Extend require parsing to recognize header specs, record aliases that point at C++ scopes, inject matching `#include`s during codegen, and cover the workflow with interop tests.

### Steps
1. Parse header libspecs in `compiler+runtime/src/jank/jank/require.jank` (and macro expansion in `compiler+runtime/src/jank/jank/ns.jank`) to recognize `["…hpp" :as alias]`, validate options, and record metadata in the namespace map.
2. Update namespace state handling (e.g., `compiler+runtime/src/jank/jank/runtime/module_loader.jank`) to resolve header paths via the module path and ensure each header is tracked for downstream compilation.
3. Teach the analyzer (`compiler+runtime/src/jank/jank/analyzer.jank`) to treat aliases marked as native headers: resolve `alias/foo` as if it were `cpp/<header-namespace>.foo` using existing `cpp` scope logic.
4. Emit includes for registered headers during codegen (`compiler+runtime/src/jank/jank/compiler/codegen_cpp.jank`), reusing the cpp/raw guard mechanism so each header appears once per translation unit.
5. Add bash and interop tests (e.g., `compiler+runtime/test/bash/...`, `compiler+runtime/test/jank/cpp/...`) exercising `(ns … (:require ["clojure/string_native.hpp" :as str-native]))`, calling functions, and ensuring regressions in legacy `cpp/raw` coverage are caught.

### Further Considerations
1. Should `require` also support `:refer` or only `:as` for headers? Option A allow both; Option B restrict to aliases.
2. Confirm how to derive the logical namespace (`clojure.string_native`) from file strings—convention or config?
3. Decide whether header discovery must fail fast if a file is missing or defer to compile-time errors.
