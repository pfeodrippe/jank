// Example: Var Registry Implementation for jank Hot-Reload
// This shows how to implement Step 2: Var Registry
//
// Location in jank codebase: include/cpp/jank/runtime/var_registry.hpp
//                           src/cpp/jank/runtime/var_registry.cpp

#pragma once

#include <unordered_map>
#include <string>
#include <functional>
#include <dlfcn.h>

namespace jank::runtime
{

  // Var entry: stores function pointer and signature
  struct var_entry
  {
    void *fn_ptr; // Current implementation
    std::string signature; // Type signature (e.g. "ii" = int(int))
    std::string module; // Module path (for hot-reload tracking)
  };

  // Global var registry
  class var_registry
  {
  public:
    static var_registry &instance()
    {
      static var_registry reg;
      return reg;
    }

    // Register a var (called during module initialization)
    void register_var(std::string const &name, void *fn_ptr, std::string const &sig)
    {
      auto &entry = vars_[name];
      entry.fn_ptr = fn_ptr;
      entry.signature = sig;
      printf("[var-registry] Registered: %s (sig: %s, ptr: %p)\n",
             name.c_str(),
             sig.c_str(),
             fn_ptr);
    }

    // Get function pointer for a var (called when invoking function)
    void *get_var(std::string const &name)
    {
      auto it = vars_.find(name);
      if(it == vars_.end())
      {
        return nullptr;
      }
      return it->second.fn_ptr;
    }

    // Load a patch module and register its symbols
    // This is the hot-reload entry point!
    int load_patch(std::string const &module_path)
    {
      printf("[var-registry] Loading patch: %s\n", module_path.c_str());

      void *handle = dlopen(module_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
      if(!handle)
      {
        printf("[var-registry] dlopen failed: %s\n", dlerror());
        return -1;
      }

      // IMPORTANT: This assumes patches export a metadata function
      // that tells us what symbols to register
      typedef struct
      {
        char const *name;
        char const *signature;
        void *fn_ptr;
      } patch_symbol;

      typedef patch_symbol *(*get_symbols_fn)(int *count);
      get_symbols_fn get_symbols = (get_symbols_fn)dlsym(handle, "jank_patch_symbols");

      if(get_symbols)
      {
        int count = 0;
        patch_symbol *symbols = get_symbols(&count);

        for(int i = 0; i < count; i++)
        {
          register_var(symbols[i].name, symbols[i].fn_ptr, symbols[i].signature);
        }

        printf("[var-registry] Loaded %d symbols from %s\n", count, module_path.c_str());
        return 0;
      }

      // Fallback: Manual symbol lookup (less flexible)
      // You'd need to know the symbol names ahead of time
      printf("[var-registry] WARNING: No jank_patch_symbols found, manual lookup required\n");
      return -1;
    }

    // Helper: Call a var with int(int) signature
    int call_int_int(std::string const &name, int arg)
    {
      void *fn = get_var(name);
      if(!fn)
      {
        printf("[var-registry] ERROR: Var not found: %s\n", name.c_str());
        return -1;
      }

      typedef int (*fn_type)(int);
      return ((fn_type)fn)(arg);
    }

  private:
    var_registry() = default;
    std::unordered_map<std::string, var_entry> vars_;
  };

} // namespace jank::runtime

// C API for WebAssembly (export these functions)
extern "C"
{
  // Register a var from C/JavaScript
  __attribute__((visibility("default"))) void
  jank_register_var(char const *name, void *fn_ptr, char const *sig)
  {
    jank::runtime::var_registry::instance().register_var(name, fn_ptr, sig);
  }

  // Load a patch module
  __attribute__((visibility("default"))) int jank_load_patch(char const *path)
  {
    return jank::runtime::var_registry::instance().load_patch(path);
  }

  // Call a var with int(int) signature
  __attribute__((visibility("default"))) int jank_call_var_int_int(char const *name, int arg)
  {
    return jank::runtime::var_registry::instance().call_int_int(name, arg);
  }

} // extern "C"

// USAGE EXAMPLE in generated code:
//
// Before (direct call):
//   int result = jank_user_my_func(42);
//
// After (indirect call through var registry):
//   int result = jank::runtime::var_registry::instance().call_int_int("user/my-func", 42);
//
// Or via C API:
//   int result = jank_call_var_int_int("user/my-func", 42);
