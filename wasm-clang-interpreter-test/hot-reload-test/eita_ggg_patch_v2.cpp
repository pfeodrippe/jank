// Hot-reload patch for eita/ggg function
// This patch changes ggg from (+ 48 v) to (+ 49 v)
//
// Compile with:
//   emcc eita_ggg_patch_v2.cpp -o eita_ggg_patch_v2.wasm -sSIDE_MODULE=1 -O2 -fPIC

#include <stdint.h>

extern "C"
{
  // Patch symbol metadata structure (must match jank's hot_reload.hpp)
  struct patch_symbol
  {
    char const *qualified_name;
    char const *signature;
    void *fn_ptr;
  };

  // Import helper functions from main module (exported by jank_box_integer, etc.)
  extern void *jank_box_integer(int64_t value);
  extern int64_t jank_unbox_integer(void *obj);

  // The patched function: implements (+ 49 v)
  // Signature: object_ref (*)(object_ref) represented as void* for C ABI
  __attribute__((visibility("default"))) void *jank_eita_ggg(void *v)
  {
    int64_t value = jank_unbox_integer(v);
    return jank_box_integer(value + 49); // Changed from 48 to 49!
  }

  // Patch metadata export - tells hot-reload which symbols to register
  __attribute__((visibility("default"))) patch_symbol *jank_patch_symbols(int *count)
  {
    static patch_symbol symbols[] = {
      { "eita/ggg", "1", (void *)jank_eita_ggg }
    };
    *count = 1;
    return symbols;
  }
}
