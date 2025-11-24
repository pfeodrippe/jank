// Minimal Hello World for WASM
#include <emscripten.h>
#include <emscripten/html5.h>

extern "C" {

EMSCRIPTEN_KEEPALIVE
void hello_from_jank() {
    emscripten_run_script("document.getElementById('output').innerHTML = '<h1>Hello World from jank WASM!</h1><p>The minimal jank runtime is working in your browser.</p>';");
}

EMSCRIPTEN_KEEPALIVE
int main() {
    hello_from_jank();
    return 0;
}

}
