Draft for review.

## Plan: Extend C++ Struct Completion

Add struct/class metadata to the existing LLVM-backed registry produced during `cpp/raw` analysis, surface those entries through the nREPL completion pipeline, and cover them with focused tests plus docs so `cpp/eita.cpp…foo.` style symbols autocomplete alongside functions.

### Steps
1. Capture struct/class declarations while processing `cpp/raw` forms in `compiler+runtime/src/cpp/jank/analyze/processor.cpp`, storing fully qualified names (and constructor hints) beside `global_cpp_functions`.
2. Expose the new struct registry via the same synchronized runtime state accessed from `nrepl-server/src/jank/nrepl/completion.clj` (near `completion-results-with-cpp`), tagging entries so clients can differentiate types vs callables.
3. Update completion result shaping in `nrepl-server/src/jank/nrepl/completion.clj` and `nrepl-server/test/jank/nrepl/completion_test.clj` so both `complete` and `completion` ops suggest struct names, constructors (`foo.`), and member-access helpers (`.-field` if applicable).
4. Document the new capability in `docs/codegen-processor-pass-order.md` or a dedicated README section, noting any naming conventions or limitations for cpp/raw-defined structs.

### Further Considerations
1. Should struct completions surface constructor suffixes (`foo.`) automatically or only bare type names?
2. How should duplicate struct names from multiple cpp/raw blocks be resolved—last definition wins or error?
3. Do we also want field/member completion (e.g., `.-a`) now, or leave that for a follow-up?
