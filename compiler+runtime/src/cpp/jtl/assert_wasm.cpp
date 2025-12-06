// Minimal WASM implementation of assert - NO standard library includes

// Declare abort without including headers
extern "C" void abort(void) __attribute__((noreturn));

// Minimal string view to avoid including headers
namespace jtl
{
  struct immutable_string_view
  {
    char const *data;
    __SIZE_TYPE__ size;
  };
}

namespace jtl
{
  void do_assertion_panic(immutable_string_view const &msg)
  {
    ::abort();
  }

  void do_assertion_throw(immutable_string_view const &msg)
  {
    // Can't throw without stdexcept, just abort
    ::abort();
  }
}
