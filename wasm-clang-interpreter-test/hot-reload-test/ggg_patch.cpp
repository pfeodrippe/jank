// Auto-generated WASM hot-reload patch
// Function: eita/ggg
// Expression: (+ 49 v)
//
// Compile with:
//   emcc ggg_patch.cpp -o ggg_patch.wasm -sSIDE_MODULE=1 -O2 -fPIC

#include <stdint.h>

extern "C"
{
  // Patch symbol metadata (must match jank's hot_reload.hpp)
  struct patch_symbol
  {
    char const *qualified_name;
    char const *signature;
    void *fn_ptr;
  };

  // Import helper functions from main module
  extern void *jank_box_integer(int64_t value);
  extern int64_t jank_unbox_integer(void *obj);

  // The patched function: eita/ggg
  // Implements: (+ 49 v)
  __attribute__((visibility("default"))) void *jank_eita_ggg(void *p0)
  {
    int64_t value = jank_unbox_integer(p0);
    return jank_box_integer(value + 49);
  }

  // Patch metadata export
  __attribute__((visibility("default"))) patch_symbol *jank_patch_symbols(int *count)
  {
    static patch_symbol symbols[] = {
      { "eita/ggg", "1", (void *)jank_eita_ggg }
    };
    *count = 1;
    return symbols;
  }
}
