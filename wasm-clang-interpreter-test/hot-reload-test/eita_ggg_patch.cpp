// Hot-reload patch for eita/ggg function
// Changes from (+ v 48) to (+ v 49)

#include <cstdint>

extern "C"
{
  // Patch symbol metadata structure
  struct patch_symbol
  {
    char const *qualified_name;
    char const *signature;
    void *fn_ptr;
  };

  // Import jank runtime functions that we need
  extern void *jank_make_box_integer(int64_t i);
  extern int64_t jank_unbox_integer(void *o);

  // Patched ggg function: changes from (+ v 48) to (+ v 49)
  __attribute__((visibility("default"))) void *jank_eita_ggg(void *v)
  {
    int64_t value = jank_unbox_integer(v);
    return jank_make_box_integer(value + 49); // Changed from 48 to 49
  }

  // Export patch metadata
  __attribute__((visibility("default"))) patch_symbol *jank_patch_symbols(int *count)
  {
    static patch_symbol symbols[] = {
      { "eita/ggg", "1", (void *)jank_eita_ggg }
    };
    *count = 1;
    return symbols;
  }
}
