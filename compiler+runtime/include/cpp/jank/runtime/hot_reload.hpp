#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

#include <jank/runtime/object.hpp>
#include <jank/runtime/var.hpp>

namespace jank::runtime
{
  /* Hot-reload registry for WASM development mode.
   *
   * When HOT_RELOAD=1 is set during build, this enables runtime function patching
   * by loading WASM side modules via dlopen and updating var bindings.
   *
   * This achieves REPL-like development speed (~180ms turnaround):
   * - Server compiles jank -> C++ -> WASM (~180ms)
   * - Browser loads patch via dlopen (~1ms)
   * - Var registry updates function bindings
   *
   * Architecture:
   *   Browser (jank.wasm MAIN_MODULE)
   *       ↓ WebSocket
   *   nREPL Server (native)
   *       - Receives eval request
   *       - Compiles to WASM side module
   *       - Sends patch to browser
   *
   * See: /hot-reload-test/README.md for proof of concept
   */
  class hot_reload_registry
  {
  public:
    static hot_reload_registry &instance();

    /* Load a WASM side module and register its symbols.
     *
     * The side module must export a jank_patch_symbols() function that returns
     * metadata about the symbols to register:
     *
     *   struct patch_symbol {
     *     const char* qualified_name;  // e.g. "user/my-func"
     *     const char* signature;       // Type signature (for validation)
     *     void* fn_ptr;                // Function pointer
     *   };
     *
     *   extern "C" patch_symbol* jank_patch_symbols(int* count);
     *
     * Returns 0 on success, -1 on error.
     */
    int load_patch(std::string const &module_path);

    /* Register a symbol from a loaded patch.
     * This creates a callable object and binds it to the var.
     */
    int
    register_symbol(std::string const &qualified_name, void *fn_ptr, std::string const &signature);

    /* Get statistics about loaded patches (for debugging). */
    struct stats
    {
      size_t loaded_modules{ 0 };
      size_t registered_symbols{ 0 };
      std::vector<std::string> module_paths;
    };

    stats get_stats() const;

  private:
    hot_reload_registry() = default;

    /* Track loaded module handles (to prevent GC). */
    struct module_info
    {
      void *handle{ nullptr };
      std::string path;
      std::vector<std::string> symbols;
    };

    std::vector<module_info> loaded_modules_;
    size_t registered_symbols_{ 0 };
  };

  /* C API for WebAssembly exports.
   * These functions are callable from JavaScript via ccall/cwrap.
   */
  extern "C"
  {
    /* Load a patch module from the virtual filesystem.
     * Path should be like "/tmp/patch_123.wasm".
     * Returns 0 on success, -1 on error.
     */
    int jank_hot_reload_load_patch(char const *path);

    /* Get statistics about loaded patches.
     * Returns JSON string (caller must free).
     */
    char const *jank_hot_reload_get_stats();

    /* Helper functions for patches to manipulate jank objects.
     * These are exported from the main module and can be called by SIDE_MODULEs.
     */

    /* Box an integer value into a jank object_ref (as void* for C ABI). */
    void *jank_box_integer(int64_t value);

    /* Unbox an integer from a jank object_ref. Returns 0 if not an integer. */
    int64_t jank_unbox_integer(void *obj);

    /* Add two boxed integers and return a new boxed integer.
     * This is a convenience function for simple arithmetic patches.
     */
    void *jank_add_integers(void *a, void *b);

    /* ===== Full runtime helpers for real jank code ===== */

    /* Call a var by namespace and name with variadic arguments.
     * This allows patches to call any function without hardcoded imports.
     *
     * Example:
     *   void* args[] = {arg1, arg2};
     *   jank_call_var("clojure.core", "+", 2, args);
     */
    void *jank_call_var(char const *ns, char const *name, int argc, void **args);

    /* Deref a var and return its current value.
     * Returns nil if var not found.
     */
    void *jank_deref_var(char const *ns, char const *name);

    /* Create a keyword (namespace can be empty string for unqualified). */
    void *jank_make_keyword(char const *ns, char const *name);

    /* Create a persistent vector from elements. */
    void *jank_make_vector(int argc, void **elements);

    /* Create a persistent set from elements. */
    void *jank_make_set(int argc, void **elements);

    /* Create a boxed double. */
    void *jank_box_double(double value);

    /* Unbox a double from a number object. Returns 0.0 if not a number. */
    double jank_unbox_double(void *obj);

    /* Create a persistent string. */
    void *jank_make_string(char const *str);

    /* Print objects (like println but returns nil). */
    void *jank_println(int argc, void **args);
  }
} // namespace jank::runtime
