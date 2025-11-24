// jank Hello World compiled to WASM
#include <emscripten.h>
#include <emscripten/html5.h>

// Include jank runtime headers
#include <jank/runtime/obj/jit_function.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>

// Generated jank code
namespace hello
{
  struct hello__main_2 : jank::runtime::obj::jit_function
  {
    jank::runtime::var_ref const hello_println_3;
    jank::runtime::obj::persistent_string_ref const hello_const_4;

    hello__main_2()
      : jank::runtime::obj::jit_function{ jank::runtime::__rt_ctx->read_string(
          "{:name \"hello/-main\"}") }
      , hello_println_3{ jank::runtime::__rt_ctx->intern_var("hello", "println").expect_ok() }
      , hello_const_4{ jank::runtime::make_box<jank::runtime::obj::persistent_string>(
          "Hello World") }
    {
    }

    jank::runtime::object_ref call() final
    {
      using namespace jank;
      using namespace jank::runtime;
      object_ref const _main{ this };
      auto const hello_call_6(jank::runtime::dynamic_call(hello_println_3->deref(), hello_const_4));
      return hello_call_6;
    }
  };
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
void run_hello() {
    // Initialize jank runtime if needed
    // Then call the generated function
    auto hello_fn = jank::runtime::make_box<hello::hello__main_2>();
    hello_fn->call();
}

int main() {
    emscripten_run_script(
        "document.getElementById('output').innerHTML = "
        "'<h1>🎉 Running jank Hello World in WASM!</h1>';"
    );
    
    // Run the jank function
    run_hello();
    
    emscripten_run_script(
        "document.getElementById('output').innerHTML += "
        "'<p><strong>✓ jank function executed successfully!</strong></p>';"
    );
    
    return 0;
}

}
