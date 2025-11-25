// jank Code Execution Demo - simulates running actual jank code
#include <emscripten.h>
#include <emscripten/html5.h>

extern "C"
{
  // Simulated jank execution - in a real implementation, this would:
  // 1. Parse the jank source code
  // 2. Compile to IR or interpret
  // 3. Execute in the jank runtime
  //
  // For now, we'll simulate the output
  EMSCRIPTEN_KEEPALIVE
  void execute_jank_code()
  {
    // Display the jank code being "executed"
    emscripten_run_script(
      "const code = `(ns my-ns)\\n\\n(println \\\"Hello World\\\") ;; Gets logged to the console`;"
      "document.getElementById('code-display').textContent = code;");

    // Simulate the execution
    emscripten_run_script(
      "document.getElementById('output').innerHTML = "
      "'<div class=\"log-entry\">🟢 <strong>Compiling:</strong> hello.jank</div>';");

    // Add delay for effect
    emscripten_run_script(
      "setTimeout(() => {"
      "  document.getElementById('output').innerHTML += "
      "  '<div class=\"log-entry\">📦 <strong>Loading namespace:</strong> my-ns</div>';"
      "}, 200);");

    emscripten_run_script("setTimeout(() => {"
                          "  document.getElementById('output').innerHTML += "
                          "  '<div class=\"log-entry\">▶️  <strong>Executing:</strong> (println "
                          "\\\"Hello World\\\")</div>';"
                          "}, 400);");

    // The actual output from println
    emscripten_run_script("setTimeout(() => {"
                          "  document.getElementById('output').innerHTML += "
                          "  '<div class=\"log-entry console-output\">Hello World</div>';"
                          "}, 600);");

    // Success message
    emscripten_run_script("setTimeout(() => {"
                          "  document.getElementById('output').innerHTML += "
                          "  '<div class=\"log-entry success\">✅ <strong>Execution completed "
                          "successfully!</strong></div>';"
                          "}, 800);");
  }

  int main()
  {
    execute_jank_code();
    return 0;
  }
}
