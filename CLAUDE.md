# Claude Code Notes for jank

## CRITICAL: Do NOT Delete llvm-install

**NEVER delete or run `rm -rf` on the `build/` directory or `build/llvm-install/`!**

The `llvm-install` folder contains the pre-built LLVM/Clang toolchain and takes hours to rebuild. If you need to clean the build:
- Only delete `build/CMakeCache.txt` and `build/CMakeFiles/`
- Or use `./bin/clean` which preserves llvm-install
- NEVER run `rm -rf build` or any command that would delete `build/llvm-install/`

## Rule 1

Always added what you learned to new .md files in the compiler+runtime/ai folder in this project!
Put them in the `20251129-001-some-time.md` format.
Increment the number for the same date, then restart on 001 in the next day!

## Building

Before building, you MUST set these environment variables:

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
export SDKROOT=$(xcrun --show-sdk-path)
export CC=$PWD/build/llvm-install/usr/local/bin/clang
export CXX=$PWD/build/llvm-install/usr/local/bin/clang++
```

Then build with:
```bash
cd build && ninja jank jank-test
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
