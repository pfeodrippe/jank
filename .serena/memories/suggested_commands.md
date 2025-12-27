# jank Build and Test Commands

## Critical Warning
**NEVER delete `build/llvm-install/`** - takes hours to rebuild!

## Environment Setup (Required Before Building)
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# IMPORTANT: Use the expanded SDKROOT path directly (command substitution may fail in some contexts)
export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
export CC=$PWD/build/llvm-install/usr/local/bin/clang
export CXX=$PWD/build/llvm-install/usr/local/bin/clang++

# Alternative: Run xcrun separately to get the path
# xcrun --show-sdk-path  # prints the SDK path, then export SDKROOT=<that path>
```

## Building
```bash
cd build && ninja jank jank-test
```

## Testing
```bash
# Full test suite (always use tee to save output for later reference!)
./build/jank-test 2>&1 | tee .tests.txt

# Or from build directory:
./jank-test 2>&1 | tee ../.tests.txt

# Specific test pattern
./build/jank-test --test-case="*pattern*"

# Run single jank file directly
./build/jank run --file path/to/test.jank
```

## Debugging with lldb
```bash
# Run tests with lldb to get backtrace on crash
lldb ./jank-test -o "run" -o "bt" -o "quit"
```

## Safe Clean (preserves llvm-install)
```bash
./bin/clean
# OR manually:
rm -f build/CMakeCache.txt
rm -rf build/CMakeFiles/
```

## Documentation Rule
Always document learnings in `compiler+runtime/ai/` with format:
`YYYYMMDD-NNN-description.md` (e.g., `20251206-002-my-feature.md`)
