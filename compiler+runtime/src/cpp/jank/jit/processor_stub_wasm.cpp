// Minimal WASM stub for JIT processor - absolutely NO includes to avoid header issues
// This file intentionally doesn't include ANY headers, not even stdexcept
// We just provide stub symbols that will never be called in a real WASM build

namespace jank::jit
{
  // Minimal stub - just enough to link
  struct processor
  {
    processor(void const *)
    {
    }

    ~processor()
    {
    }
  };
}
