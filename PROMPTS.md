# Prompts

## Prompt 1 (November 25, 2025)

Don't generate C++ code, generate LLVM IR to WASM as the C++ generation is broken!! Also, I've exported some env vars and the native build should be working now. Go, iterate! Fix it!!

Use eita.jank as your test file, we shoukld make it work so we can have the run working cleanly just like it does using the native jank. 

Don't use /dev/null nor /tmp (create temp files inside the repo as needed) !!

Ok, focus! You have a libjank.a for wasm, correct? You have the clojure.core.o object for wasm as well? If not generate it (this exists for the native one after phase 1, create this!!!)

----

## Prompt 2 (November 25, 2025)

Don't generate C++ (it's broken), genereate llvm IR!!

----

## Prompt 3 (November 25, 2025)

Add a one-liner (or add to the emscripten-bundle) so we just run it just by passing the .jank file.

Also now, run the real eita2.jank, we have a `refer` inside clojure/core.jank, so it should work!! Fix it

----

## Prompt 4 (November 25, 2025)

No, don't modify core_native to add refer!! The WASM should use the clojure/core.jank one!! Check how the native one is using and replicate so we can use it from the WASM side!!