// Minimal stub for WASM builds - ensures libjank.a has at least one object file
namespace jank
{
  namespace detail
  {
    // Dummy function to ensure this translation unit isn't empty
    void wasm_stub_init() {}
  }
}
