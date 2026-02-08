# Remaining Merge Conflicts - Resolution Guide

## Completed: 9/42 files ✅

1. ✅ `include/cpp/clojure/core_native.hpp` - const + WASM functions
2. ✅ `include/cpp/clojure/string_native.hpp` - const + WASM string functions
3. ✅ `include/cpp/jank/analyze/expr/call.hpp` - const + return_tag_type
4. ✅ `include/cpp/jank/analyze/expr/var_deref.hpp` - const + tag_type
5. ✅ `include/cpp/jank/codegen/processor.hpp` - ALL functions from both branches
6. ✅ `include/cpp/jank/error/analyze.hpp` - const qualifiers
7. ✅ `include/cpp/jank/jit/processor.hpp` - Both eval_string overloads
8. ✅ `include/cpp/jank/runtime/context.hpp` - option<> returns + CLI opts + bdwgc workaround
9. ✅ `include/cpp/jank/runtime/convert/builtin.hpp` - object_ref returns

## Remaining: 33/42 files

### Critical Pattern for ALL Remaining Files:

**Golden Rule**: MERGE both sides, NEVER choose one
- nrepl-4: iOS/WASM/nREPL conditionals + new features
- origin/main: Refactorings + const qualifiers + performance
- **Resolution**: Keep BOTH

### Common Conflict Patterns:

#### Pattern 1: Adding `const` qualifiers (80% of conflicts)
```cpp
<<<<<<< HEAD
object_ref foo(object_ref param);
=======
object_ref foo(object_ref const param);
>>>>>>> origin/main
```
**Resolution**: Accept origin/main (add `const`)

#### Pattern 2: Return type changes (object_ref vs jtl::option<object_ref>)
```cpp
<<<<<<< HEAD
object_ref eval_file(...);
=======
jtl::option<object_ref> eval_file(...);
>>>>>>> origin/main
```
**Resolution**: Use `jtl::option<object_ref>` (better error handling)

#### Pattern 3: iOS/WASM conditionals
```cpp
<<<<<<< HEAD
#ifdef JANK_TARGET_IOS
  // iOS-specific code
#endif
void common_function();
=======
void common_function();
void new_function();  // origin/main adds this
>>>>>>> origin/main
```
**Resolution**: Keep BOTH - conditionals AND new function

#### Pattern 4: Function additions (both sides add different functions)
```cpp
<<<<<<< HEAD
void nrepl_specific_function();
=======
void refactored_function();
>>>>>>> origin/main
```
**Resolution**: Keep BOTH functions

### Remaining Headers (6 files):

10. **`include/cpp/jank/runtime/core.hpp`** (2 conflicts)
    - Likely: const qualifiers + maybe new core functions
    - Strategy: Add const, keep all functions

11. **`include/cpp/jank/runtime/core/math.hpp`** (1 conflict)
    - Likely: const qualifiers
    - Strategy: Add const

12. **`include/cpp/jank/runtime/ns.hpp`** (1 conflict)
    - Likely: const qualifiers or new ns functions
    - Strategy: Merge both

13. **`include/cpp/jank/runtime/oref.hpp`** (4 conflicts) ⚠️ COMPLEX - 641 lines
    - Template specializations conflicts
    - Strategy: Carefully merge each template specialization

14. **`include/cpp/jank/util/cli.hpp`** (4 conflicts)
    - CLI options for iOS/nREPL
    - Strategy: Keep all options from both branches

15. **`include/cpp/jtl/immutable_string.hpp`** (5 conflicts) ⚠️ COMPLEX - 973 lines
    - Template and operator overloads
    - Strategy: Carefully merge operators and templates

### Source Files (20 files):

All source files will follow similar patterns - implementation of the header changes.

Key ones:
- **`src/cpp/clojure/core_native.cpp`** - Implements WASM functions + const changes
- **`src/cpp/jank/runtime/context.cpp`** - Implements eval_string overload + CLI opts
- **`src/cpp/jank/jit/processor.cpp`** - Implements eval_string_with_result
- **`src/cpp/main.cpp`** - CLI handling changes

### Other Files (7 files):

- **`../.gitignore`** - Simple merge of ignore patterns
- **`src/jank/clojure/core.jank`** - New core functions (Clojure code)
- **`test/bash/clojure-test-suite/clojure-test-suite`** - Submodule or test script
- **`test/cpp/main.cpp`** - Test harness changes
- **`../lein-jank/src/leiningen/jank.clj`** - Leiningen plugin changes

## Resolution Checklist:

For each remaining file:
1. ✅ Read the conflict
2. ✅ Identify the pattern (const, return type, conditionals, or function additions)
3. ✅ Apply MERGE strategy (never choose one side)
4. ✅ Verify no conflict markers remain
5. ✅ Move to next file

## Estimated Effort:
- Simple files (const only): ~1 min each = 15 files = 15 min
- Medium files (multiple patterns): ~3 min each = 12 files = 36 min
- Complex files (oref.hpp, immutable_string.hpp): ~10 min each = 2 files = 20 min
- **Total: ~71 minutes**

## Success Criteria:
```bash
git status --short | grep "^UU" | wc -l
# Should return: 0
```
