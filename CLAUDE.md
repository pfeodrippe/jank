# Claude Code Notes for jank

## CRITICAL 1: Do NOT Delete llvm-install

## CRITICAL 2: Always run the tests before changes (./bin/test), put result into a .tests.txt file so you can refer later when you do the changes to make sure you haven't broke any new test!

## CRITICAL 3: Before finalizing, read .tests.txt, run the tests again (./bin/test) and compare so you don't have new breaks!

**NEVER delete or run `rm -rf` on the `build/` directory or `build/llvm-install/`!**

The `llvm-install` folder contains the pre-built LLVM/Clang toolchain and takes hours to rebuild. If you need to clean the build:
- Only delete `build/CMakeCache.txt` and `build/CMakeFiles/`
- Or use `./bin/clean` which preserves llvm-install
- NEVER run `rm -rf build` or any command that would delete `build/llvm-install/`

## Rule 1

Always added what you learned to new .md files in the compiler+runtime/ai folder in this project!
Put them in the `20251129-001-some-time.md` format.
Increment the number for the same date, then restart on 001 in the next day!

## Rule 2: NEVER do manual operations - always update scripts!

When you need to copy files, run build steps, or do any repeatable operation:
- **NEVER** copy files manually (e.g., `cp libjank.a somewhere/`)
- **NEVER** run one-off commands that should be part of a build process
- **ALWAYS** update the Makefile or build script to handle the operation automatically
- If a script/Makefile is missing a step, ADD IT to the script

This ensures:
1. The operation is reproducible
2. Other developers don't have to figure out manual steps
3. CI/CD will work correctly
4. The user won't have to remember manual steps

Example: If you need to copy libjank.a to a directory for Xcode, add a Makefile target instead of running `cp` directly.

## Rule 3: Use tee for CLI commands

When running CLI commands (especially build scripts, tests, or any commands with significant output), always use `tee` to save the output to a file so the user can see the full output. This is especially useful for debugging failures.

Example:
```bash
make sdf-ios-simulator-run 2>&1 | tee build-output.txt
```

This way:
1. The user can review the full output after the conversation
2. You can refer to the file if something fails
3. The output is preserved for later analysis

## Building

**CRITICAL: Use these EXACT commands with hardcoded paths (no command substitution like `$(...)` which can fail):**

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk CC=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang CXX=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++ ninja -C build jank jank-test
```

If you need to reconfigure:
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk CC=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang CXX=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++ ./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on
```

## Running Tests

```bash
./build/jank-test
```

To run specific test cases:
```bash
./build/jank-test --test-case="*pattern*"
```

## nREPL Tests

nREPL tests are in `/Users/pfeodrippe/dev/jank/compiler+runtime/test/cpp/jank/nrepl/engine.cpp`
- To fix pch header corruption, use `./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on && ./bin/compile`
