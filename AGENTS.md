## Agent Capabilities

- This agent can build and test the jank compiler+runtime by following
	`.claude/skills/jank-build-and-test/SKILL.md`.

## Rules

- **Never use `/dev/null`** - Do not redirect output to `/dev/null`. Always capture and show output for debugging purposes.
- Put the raw prompt inside a PROMPTS.md file, always update it with exactly my instructions (and nothing else!), append each new instruction to the end
- For the jank -> wasm work, also read AGENTS_CONTEXT.md. In the end, add the output for a given instruction for what was learnt (one for each thinking, context, barriers, discoveries, ideas you have, commands you have learnt etc) 

## Command Log

### Build

```
export SDKROOT=$(xcrun --show-sdk-path); export CC=$PWD/build/llvm-install/usr/local/bin/clang; export CXX=$PWD/build/llvm-install/usr/local/bin/clang++
./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on # This only need to be called once and then you can just call ./bin/compile
./bin/compile
./bin/compile && bin/jank/compiler+runtime/bash_test.clj && ./bin/test
```

### Test
- `./bin/test`
- `lein jank run 5555`
- `cd test/bash/clojure-test-suite && PATH="/Users/pfeodrippe/dev/jank/compiler+runtime/build:$PATH" ./pass-test; cd -`

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
