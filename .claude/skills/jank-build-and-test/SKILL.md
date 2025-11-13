---
name: jank-build-and-test
description: Build and test the jank compiler+runtime; use when running ./bin/test or the ahead-of-time Babashka suite.
---

# Jank Build And Test

## Context
- Applies inside the `compiler+runtime` workspace for the jank project.
- Relies on project-provided scripts in `compiler+runtime/bin/` and Babashka tasks in `compiler+runtime/test/bash/ahead-of-time`.
- Expect verbose `dyld` warnings on macOS during AOT runs; they have not affected test outcomes so far.

## Instructions
1. **Run the full C++ and Jank test suite once**
   - From `compiler+runtime/`, execute `./bin/test`.
   - Use `./bin/test <path-to-test>` to target a specific Jank test file, e.g. `./bin/test test/jank/reader-macro/symbolic-value/fail-bad-nan.jank`.
2. **Continuously watch and rerun tests during development**
   - Start `./bin/watch ./bin/test` from `compiler+runtime/` to rebuild and retest on file changes.
3. **Exercise ahead-of-time (AOT) Bash harnesses**
   - `cd compiler+runtime/test/bash/ahead-of-time` and run `bb pass-test`.
   - The task builds the CLI binary into `target/cli`, executes the scripted scenarios, and compares results with `expected-output/` fixtures (e.g. `expected-output/cpp-raw-inline/core`).
   - On success you should see `Ran 5 tests containing 14 assertions. 0 failures, 0 errors.`; any mismatch shows the expected vs. actual output to help diagnose regressions.
4. **Triaging AOT failures**
   - Investigate `expected-output` diffs before updating fixtures; earlier output drift ("11" vs "10") proved the harness correctly flags regressions.
   - After fixes, rerun `bb pass-test` until the suite is green and the `target/` directory is cleaned automatically.

For compiling Jank (from the `compiler+runtime` folder)

``` bash
./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on
./bin/compile
```

For debugging the cpp or llvm output of a .jank file

``` bash
# /Users/pfeodrippe/dev/jank/compiler+runtime/test/bash/ahead-of-time

# CPP
jank --codegen cpp run src/cpp_raw_inline/core.jank

# LLVM (compile, more useful)
jank --codegen llvm_ir --module-path="$(clojure -A:cpp-raw-inline -Spath)" compile cpp_raw_inline.core

# LLVM (seeing the output, less useful)
jank --codegen llvm_ir --module-path="$(clojure -A:cpp-raw-inline -Spath)" run-main cpp_raw_inline.core
```

## Examples
```bash
# Full suite from compiler+runtime/
./bin/test

# Focused single test
./bin/test test/jank/reader-macro/symbolic-value/fail-bad-nan.jank

# Watch mode (press Ctrl+C to exit)
./bin/watch ./bin/test

# Ahead-of-time bash harness
cd compiler+runtime/test/bash/ahead-of-time
export PATH="/Users/pfeodrippe/dev/jank/compiler+runtime/build:$PATH"
bb pass-test | grep -v "missing from root that overrides"
```

## Troubleshooting
- Large blocks of `dyld` warnings during `bb pass-test` have not impacted the pass/fail signal; focus on the final summary lines.
- If an `expected-output` file truly changes, adjust the fixture and rerun the suite to confirm the update is justified.
