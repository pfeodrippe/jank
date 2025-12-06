# jank Build and Test Commands

## Critical Warning
**NEVER delete `build/llvm-install/`** - takes hours to rebuild!

## Environment Setup (Required Before Building)
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
export SDKROOT=$(xcrun --show-sdk-path)
export CC=$PWD/build/llvm-install/usr/local/bin/clang
export CXX=$PWD/build/llvm-install/usr/local/bin/clang++
```

## Building
```bash
cd build && ninja jank jank-test
```

## Testing
```bash
# Full test suite
./build/jank-test

# Specific test pattern
./build/jank-test --test-case="*pattern*"
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
