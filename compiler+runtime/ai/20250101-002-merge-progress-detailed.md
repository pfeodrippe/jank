# Merge Resolution Progress - Detailed Status (2026-01-01)

## COMPLETED Resolutions (6 files)
✅ 1. `include/cpp/clojure/core_native.hpp` - Added `const` qualifiers + kept all WASM/native functions
✅ 2. `include/cpp/clojure/string_native.hpp` - Added `const` qualifiers + kept all WASM string functions
✅ 3. `include/cpp/jank/analyze/expr/call.hpp` - Added `const` + kept `return_tag_type` parameter
✅ 4. `include/cpp/jank/analyze/expr/var_deref.hpp` - Added `const` + kept `tag_type` parameter
✅ 5. `include/cpp/jank/codegen/processor.hpp` - Kept ALL functions from both branches
✅ 6. `include/cpp/jank/error/analyze.hpp` - Added `const` qualifiers to all error functions

## REMAINING (36 files)

### Headers (9 remaining):
7. `include/cpp/jank/jit/processor.hpp` (1 conflict)
8. `include/cpp/jank/runtime/context.hpp` (2 conflicts) - 264 lines
9. `include/cpp/jank/runtime/convert/builtin.hpp` (3 conflicts)
10. `include/cpp/jank/runtime/core.hpp` (2 conflicts) - 171 lines
11. `include/cpp/jank/runtime/core/math.hpp` (1 conflict)
12. `include/cpp/jank/runtime/ns.hpp` (1 conflict)
13. `include/cpp/jank/runtime/oref.hpp` (4 conflicts) - 641 lines COMPLEX
14. `include/cpp/jank/util/cli.hpp` (4 conflicts) - 161 lines
15. `include/cpp/jtl/immutable_string.hpp` (5 conflicts) - 973 lines COMPLEX

### Source Files (20 remaining):
16. `src/cpp/clojure/core_native.cpp`
17. `src/cpp/jank/analyze/cpp_util.cpp`
18. `src/cpp/jank/analyze/processor.cpp`
19. `src/cpp/jank/aot/processor.cpp`
20. `src/cpp/jank/c_api.cpp`
21. `src/cpp/jank/codegen/llvm_processor.cpp`
22. `src/cpp/jank/codegen/processor.cpp`
23. `src/cpp/jank/compiler_native.cpp`
24. `src/cpp/jank/evaluate.cpp`
25. `src/cpp/jank/jit/processor.cpp`
26. `src/cpp/jank/read/parse.cpp`
27. `src/cpp/jank/read/reparse.cpp`
28. `src/cpp/jank/runtime/context.cpp`
29. `src/cpp/jank/runtime/core/meta.cpp`
30. `src/cpp/jank/runtime/obj/big_decimal.cpp`
31. `src/cpp/jank/runtime/obj/big_integer.cpp`
32. `src/cpp/jank/runtime/obj/transient_array_map.cpp`
33. `src/cpp/jank/runtime/obj/transient_sorted_set.cpp`
34. `src/cpp/jank/runtime/var.cpp`
35. `src/cpp/jank/util/cli.cpp`
36. `src/cpp/jtl/string_builder.cpp`
37. `src/cpp/main.cpp`

### Other (5 remaining):
38. `../.gitignore`
39. `src/jank/clojure/core.jank`
40. `test/bash/clojure-test-suite/clojure-test-suite`
41. `test/cpp/main.cpp`
42. `../lein-jank/src/leiningen/jank.clj`

## Merge Strategy Pattern

### Most Common Pattern (90% of conflicts):
- **nrepl-4**: Adds iOS/WASM/nREPL conditional compilation (`#ifdef JANK_TARGET_IOS`)
- **origin/main**: Code refactorings, adds `const` qualifiers, performance improvements
- **Resolution**: MERGE both - keep conditionals AND apply refactorings

### Special Cases:
- **Large template files** (`oref.hpp`, `immutable_string.hpp`): Multiple template specializations conflicts
- **Build scripts**: Keep all platform-specific logic
- **.jank files**: Likely adds new core functions

## Next Actions:
Continue resolving remaining 36 files following the established merge pattern.
