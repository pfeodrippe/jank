## Agent Capabilities

- This agent can build and test the jank compiler+runtime by following
	`.claude/skills/jank-build-and-test/SKILL.md`.

## Command Log

### Build
- `export SDKROOT=$(xcrun --show-sdk-path); export CC=$PWD/build/llvm-install/usr/local/bin/clang; export CXX=$PWD/build/llvm-install/usr/local/bin/clang++`
- `./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on # This only need to be called once and then you can just call ./bin/compile`
- `./bin/compile`
- `./bin/compile && bin/jank/compiler+runtime/bash_test.clj && ./bin/test`

### Test
- `./bin/test`
- `lein jank run 5555`

### Diagnostics & Investigation
- `otool -L /Users/pfeodrippe/dev/jank/compiler+runtime/build/jank | head`
- `grep -n "Undefined" -n /tmp/jank_nrepl.log`
- `grep -n "ld:" /tmp/jank_nrepl.log`
- `ls /usr/lib/libc++*.dylib`
- `ls /usr/lib/libc++.1.0.dylib`
- `python3 - <<'PY'\nimport ctypes\nctypes.CDLL('libc++.1.dylib')\nprint('ok')\nPY`
- `ls /opt/homebrew/opt/llvm/lib/libc++*`
- `python3 - <<'PY'\nimport os\nprint(os.path.exists('/usr/lib/libc++.1.dylib'))\nPY`
- `python3 - <<'PY'\nimport ctypes\ntry:\n    ctypes.CDLL('libc++.dylib')\n    print('libc++.dylib ok')\nexcept OSError as e:\n    print('libc++.dylib fail', e)\ntry:\n    ctypes.CDLL('libc++.1.dylib')\n    print('libc++.1.dylib ok')\nexcept OSError as e:\n    print('libc++.1.dylib fail', e)\nPY`
