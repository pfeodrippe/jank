// Hot-reload test main module
// Demonstrates jank-style var registry with dynamic function replacement

#include <stdio.h>
#include <dlfcn.h>
#include <emscripten.h>

// Var registry - function pointer that can be hot-swapped
typedef int (*ggg_fn)(int);
ggg_fn ggg_impl = nullptr;

extern "C"
{
  // Register a new implementation of ggg
  EMSCRIPTEN_KEEPALIVE
  int register_ggg(void *fn_ptr)
  {
    ggg_impl = (ggg_fn)fn_ptr;
    printf("Registered new ggg implementation at %p\n", fn_ptr);
    return 0;
  }

  // Call ggg through the var registry (indirect call)
  EMSCRIPTEN_KEEPALIVE
  int call_ggg(int v)
  {
    if(!ggg_impl)
    {
      printf("ERROR: ggg not registered!\n");
      return -1;
    }
    return ggg_impl(v);
  }

  // Load a WASM side module and extract jank_ggg
  EMSCRIPTEN_KEEPALIVE
  int load_module(char const *path)
  {
    printf("Loading module: %s\n", path);

    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if(!handle)
    {
      printf("dlopen failed: %s\n", dlerror());
      return -1;
    }

    void *sym = dlsym(handle, "jank_ggg");
    if(!sym)
    {
      printf("dlsym failed: %s\n", dlerror());
      dlclose(handle);
      return -1;
    }

    // Register the new implementation
    register_ggg(sym);
    return 0;
  }

  // Test function
  EMSCRIPTEN_KEEPALIVE
  void run_test()
  {
    printf("\n=== Hot Reload Test ===\n");
    printf("call_ggg(10) = %d\n", call_ggg(10));
    printf("Expected: 58 (v1: 10+48) or 59 (v2: 10+49)\n");
  }
}

int main()
{
  printf("Hot-reload main module initialized\n");
  printf("Use load_module() to load side modules\n");
  return 0;
}
