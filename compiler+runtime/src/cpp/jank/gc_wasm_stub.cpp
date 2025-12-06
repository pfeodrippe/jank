// WASM stub for GC_throw_bad_alloc
// The BDWGC WASM build doesn't include gc_badalc.cc, so we provide a stub

#include <cstdlib>
#include <stdexcept>

// GC_throw_bad_alloc is called when GC allocation fails
// It's declared in gc_cpp.h as a C++ function (no extern "C")
void GC_throw_bad_alloc()
{
  // In WASM, we can't really recover from OOM gracefully
  // Just throw a standard bad_alloc
  throw std::bad_alloc();
}
