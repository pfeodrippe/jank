// Minimal WASM implementation of panic - NO standard library includes
// We use compiler builtins instead of standard library functions

// Declare abort without including headers
extern "C" void abort(void) __attribute__((noreturn));

namespace jtl::detail
{
  void panic(char const * const msg)
  {
    // In WASM we can't reliably print, so just abort
    // A real implementation could use emscripten_console_log
    ::abort();
  }
}
