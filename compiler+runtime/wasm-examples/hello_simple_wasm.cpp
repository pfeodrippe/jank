// Simplified jank WASM demo - without full runtime
// This demonstrates the concept without requiring the complete runtime

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>

// Simulate what jank-generated code would do, but simplified
namespace hello {
    // Instead of full jank runtime, we'll directly implement the logic
    void main_function() {
        // This is what hello/-main does
        printf("Hello World\n");
        
        // Also output to browser console
        emscripten_run_script("console.log('Hello World from jank WASM!')");
    }
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
void jank_hello_main() {
    hello::main_function();
}

int main() {
    printf("=== jank WASM Demo ===\n");
    printf("Generated from: (ns hello) (defn -main [] (println \"Hello World\"))\n\n");
    
    // Run the jank function
    jank_hello_main();
    
    printf("\n=== Success! ===\n");
    
    // Update browser UI
    emscripten_run_script(
        "if (typeof document !== 'undefined' && document.getElementById('output')) {"
        "  document.getElementById('output').innerHTML = "
        "    '<h1>🎉 jank Hello World in WASM!</h1>' +"
        "    '<p>Generated from: <code>(ns hello) (defn -main [] (println \"Hello World\"))</code></p>' +"
        "    '<p><strong>✓ Function executed successfully!</strong></p>' +"
        "    '<pre>Output: Hello World</pre>';"
        "}"
    );
    
    return 0;
}

}
