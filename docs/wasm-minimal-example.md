# Minimal jank → WebAssembly (emscripten) plan

Goal: render a browser page that runs a wasm module produced from `(println "hello from jank")` without rewriting the entire compiler toolchain. We lean on emscripten so we can reuse its libc++, JS glue, and `emrun`/`emcmake` workflow.

## Snapshot of current blockers
- `compiler+runtime/CMakeLists.txt` now exposes `jank_target_wasm` and accepts `CMAKE_SYSTEM_NAME=Emscripten`, but most libraries still assume a POSIX platform (folly, OpenSSL, BDWGC defaults) so the wasm build fails immediately after configuration.
- `runtime::context` (`compiler+runtime/src/cpp/jank/runtime/context.cpp`) always creates the full JIT stack (`jit_prc`, `clang::Interpreter`, perf plugins, Mach-O archive loading). Emscripten lacks `dlopen`, signal handlers, and RWX pages, so this code path must be compiled out or stubbed even though the new `jit/processor_stub.cpp` prevents link errors.
- `runtime::module::loader` still assumes it can evaluate source and dlopen objects at runtime. Under emscripten the loader now throws a descriptive error, but we still need a precompiled module registry for the final bundle.
- `runtime::module::loader` still assumes it can evaluate source and dlopen objects at runtime. Under emscripten the loader now throws a descriptive error, but we still need a precompiled module registry for the final bundle.
- `util::default_target_triple()` now short-circuits to `wasm32-unknown-emscripten`, but we still need `context::write_module`/`aot::processor` to inject the right em++ flags.
- `context::write_module` and `compile_module` now fail fast under emscripten; a host build must pre-generate all object files before bundling the wasm runtime.
- `aot::processor` (`compiler+runtime/src/cpp/jank/aot/processor.cpp`) injects `JANK_AOT_FLAGS` plus `-lLLVM -lclang-cpp -lcrypto -lz -lzstd -lm -lstdc++`. None of those prebuilt libs exist in emsdk, so we need a dedicated flag set (likely just `-sSTANDALONE_WASM -sALLOW_MEMORY_GROWTH` + the emscripten system libs).
- BDWGC is currently built for mac/linux with pthreads. Its WebAssembly mode (`third-party/bdwgc/include/private/gcconfig.h`, `docs/platforms/README.emscripten`) expects cooperative stack switching and disabled threads, so the CMake wrapper has to toggle the right defines and skip the pthread build.
- Optional deps (`cpptrace`, `folly`, `ftxui`, OpenSSL) drag in pthreads, futexes, and syscalls that are unavailable in the browser. FTXUI and cpptrace are now automatically excluded when `jank_target_wasm=on`, but folly/OpenSSL still assume a host OS. For the minimal demo we must eventually compile a tiny `libjank-mini` that only keeps the runtime pieces needed to evaluate precompiled forms.

## Recent progress
- `compiler+runtime/CMakeLists.txt` gained `option(jank_target_wasm ...)`, Emscripten-specific linker tweaks, and automatic exclusion of FTXUI/cpptrace when the wasm flag is set.
- `include/cpp/jank/util/cpptrace.hpp` provides a shim so wasm builds compile without pulling in cpptrace.
- `src/cpp/jank/jit/processor_stub.cpp` replaces the Clang REPL when targeting emscripten, preventing `dlopen`/perf code from linking in that configuration.
- Added the scaffolded `compiler+runtime/bin/emscripten-bundle` helper as the entry point for future host→wasm build orchestration.
- Guarded the runtime module loader so wasm builds fail fast when attempting to `load-o`, `load-cpp`, or `load-jank`, forcing us to rely on precompiled modules.
- Overrode `util::default_target_triple()` so all target/triple consumers see `wasm32-unknown-emscripten` during wasm builds.
- `context::compile_module`/`write_module` now short-circuit on wasm builds, making the requirement to precompile modules explicit.
- `jank_lib_sources` drops all codegen/AOT/nREPL/native-source files when `jank_target_wasm=on`, shrinking the dependency surface to pieces that actually exist in the wasm runtime.

## Proposed emscripten pipeline
1. **Toolchain bootstrap**
	 - Install emsdk 3.1.73 (same version `third-party/cppinterop/Emscripten-build-instructions.md` uses):
		 ```bash
		 git clone https://github.com/emscripten-core/emsdk.git
		 ./emsdk install 3.1.73
		 ./emsdk activate 3.1.73
		 source emsdk/emsdk_env.sh
		 ```
	 - Build the experimental wasm-aware CppInterOp/Clang-REPL by following `third-party/cppinterop/Emscripten-build-instructions.md`. This produces `libclang_repl_wasm.a` and friends that we can point jank at if we later turn JIT back on.

2. **CMake support for `CMAKE_SYSTEM_NAME=Emscripten`**
	 - Extend the OS dispatch block in `compiler+runtime/CMakeLists.txt` so `Emscripten` selects `-sSIDE_MODULE=1` style flags instead of erroring. *(Done: the configure step now succeeds with `emcmake`.)*
	 - Introduce `option(jank_target_wasm "Build for wasm32-unknown-emscripten" OFF)` which toggles:
		 - `jank_enable_phase_2` OFF (phase 2 libs rely on host linker scripts). *(Done.)*
		 - `find_package(OpenSSL ...)` guarded behind `if(NOT jank_target_wasm)`. *(Done, though we still need to remove downstream link usages.)*
		 - Replacement of `jank_link_whole_start`/`end` with empty strings (emscripten archives are handled differently). *(Done.)*

3. **Shrink wrapped runtime**
	 - Add `JANK_TARGET_EMSCRIPTEN` define that disables features which expect OS support:
		 - Skip `jit_prc` construction and instead load only the modules compiled ahead-of-time on the host.
		 - Stub out `perf`/`cpptrace` integration and any syscall-heavy utilities.
	 - Build BDWGC with `-DWEBASSEMBLY=1 -DENABLE_THREADS=OFF -DUSE_MMAP=0` per `third-party/bdwgc/docs/platforms/README.emscripten`.

4. **Host-side compilation flow**
	 - On a mac/Linux dev box, run:
		 ```bash
		 ./build/jank compile --codegen llvm_ir --runtime static -o hello-native hello
		 ```
		 to populate the binary cache with `hello`’s IR/object file.
	- Invoke the scaffolded helper (`compiler+runtime/bin/emscripten-bundle`) that:
		 1. Sets `JANK_TARGET_EMSCRIPTEN=1` and reconfigures with `emcmake cmake -Djank_target_wasm=on ...`.
		 2. Calls `emcmake cmake --build` to produce `libjank-mini-wasm.a` plus the cached module object.
		 3. Generates an entrypoint C file via the existing `gen_entrypoint` logic but forces `--target=wasm32-unknown-emscripten` and replaces the linker flags with emscripten ones (e.g. `em++ entrypoint.cpp libjank-mini-wasm.a -sALLOW_MEMORY_GROWTH -sEXPORTED_RUNTIME_METHODS=ccall -sEXPORTED_FUNCTIONS=_jank_entrypoint`).

5. **Browser harness**
	 - Produce `hello.html/hello.js/hello.wasm` by invoking `em++` with `-sMODULARIZE -sEXPORT_ES6=1 --shell-file docs/templates/jank-shell.html`.
	 - Minimal JS glue:
		 ```js
		 import createModule from './hello.js';
		 createModule().then((Module) => {
			 Module.ccall('jank_entrypoint', 'number', ['number', 'number'], [0, 0]);
		 });
		 ```
	 - Serve via `emrun --no_browser hello.html` or any static web server; confirm the browser console shows `hello from jank`.

## Implementation checklist
- [ ] Gate unsupported deps (`OpenSSL`, `cpptrace`, `folly`, `ftxui`) behind `if(NOT jank_target_wasm)`. *(FTXUI + cpptrace done; OpenSSL partially gated; folly still unconditional.)*
- [ ] Teach `context::write_module` and `aot::processor` to honor `JANK_TARGET_EMSCRIPTEN` by substituting `wasm32-unknown-emscripten` for `util::default_target_triple()` and swapping in emscripten link flags.
- [ ] Create `bin/emscripten-bundle` helper that orchestrates host compilation + emscripten build + html glue emission. *(Scaffold exists; needs real build steps.)*
- [ ] Add an integration test that runs `node --experimental-wasm-return-call-export hello.js` to ensure the wasm prints in a headless JS runtime.

## Risks / open items
- The JIT-free runtime path needs thorough auditing; macros and eval features may still try to emit new code at runtime.
- BDWGC on wasm is single-threaded and may require manual stack roots for async callbacks; we should start with a small heap and leak detection off.
- CppInterOp’s wasm backend is explicitly marked "experimental"; for the minimal demo we should avoid touching it entirely.
- Browser `console.log` mirroring requires piping stdout through `emscripten_log`, so we must confirm the runtime’s `println` ultimately writes to `stdout` (it currently does via `fmt::print`, which maps to `printf`, so it should work once libc hooks are in place).
