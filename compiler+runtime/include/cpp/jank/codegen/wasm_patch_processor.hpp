#pragma once

#include <jtl/ptr.hpp>
#include <jtl/string_builder.hpp>

#include <jank/analyze/expression.hpp>
#include <jank/analyze/expr/def.hpp>

/* Forward declarations for expression types */
namespace jank::analyze::expr
{
  using primitive_literal_ref = jtl::ref<struct primitive_literal>;
  using call_ref = jtl::ref<struct call>;
  using local_reference_ref = jtl::ref<struct local_reference>;
  using var_deref_ref = jtl::ref<struct var_deref>;
  using vector_ref = jtl::ref<struct vector>;
  using set_ref = jtl::ref<struct set>;
  using do_ref = jtl::ref<struct do_>;
  using let_ref = jtl::ref<struct let>;
  using if_ref = jtl::ref<struct if_>;
  using function_ref = jtl::ref<struct function>;
}

namespace jank::codegen
{
  /* Generates C++ code for WASM SIDE_MODULE patches (hot-reload).
   *
   * Unlike the regular processor, this generates:
   * - extern "C" functions (no C++ namespaces or structs)
   * - Runtime helper calls (jank_box_integer, jank_call_var, etc.)
   * - patch_symbol metadata for the hot-reload registry
   * - Unique symbol names to avoid Emscripten dlsym caching
   *
   * The generated code can be compiled with:
   *   emcc patch.cpp -o patch.wasm -sSIDE_MODULE=1 -O2 -fPIC
   */
  struct wasm_patch_processor
  {
    wasm_patch_processor(analyze::expr::def_ref const &def_expr,
                         jtl::immutable_string const &ns_name,
                         size_t patch_id);

    /* Generate the full patch C++ code. */
    jtl::immutable_string generate();

  private:
    /* Generate the runtime helper imports. */
    void gen_imports();

    /* Generate the patch_symbol struct definition. */
    void gen_patch_symbol_struct();

    /* Generate code for an expression. */
    jtl::immutable_string gen_expr(analyze::expression_ref const &expr);

    /* Generate code for specific expression types. */
    jtl::immutable_string gen_primitive_literal(analyze::expr::primitive_literal_ref const &expr);
    jtl::immutable_string gen_call(analyze::expr::call_ref const &expr);
    jtl::immutable_string gen_local_reference(analyze::expr::local_reference_ref const &expr);
    jtl::immutable_string gen_var_deref(analyze::expr::var_deref_ref const &expr);
    jtl::immutable_string gen_vector(analyze::expr::vector_ref const &expr);
    jtl::immutable_string gen_set(analyze::expr::set_ref const &expr);
    jtl::immutable_string gen_do(analyze::expr::do_ref const &expr);
    jtl::immutable_string gen_let(analyze::expr::let_ref const &expr);
    jtl::immutable_string gen_if(analyze::expr::if_ref const &expr);
    jtl::immutable_string gen_function(analyze::expr::function_ref const &expr);

    /* Helper to generate a unique variable name. */
    jtl::immutable_string fresh_local();

    /* Helper to munge a jank name to a valid C identifier. */
    static jtl::immutable_string munge_name(jtl::immutable_string const &name);

    analyze::expr::def_ref def_expr_;
    jtl::immutable_string ns_name_;
    size_t patch_id_;
    size_t local_counter_{ 0 };
    size_t anon_fn_counter_{ 0 };
    jtl::string_builder output_;
    jtl::string_builder anon_fns_; // Accumulates anonymous function definitions
    jtl::string_builder *current_output_{ &output_ }; // Active output buffer (switches for nested fns)
  };
} // namespace jank::codegen
