# Claude Code Notes for jank

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
