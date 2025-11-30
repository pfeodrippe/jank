#include <cmath>

#include <jank/codegen/wasm_patch_processor.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/analyze/visit.hpp>
#include <jank/analyze/expr/primitive_literal.hpp>
#include <jank/analyze/expr/call.hpp>
#include <jank/analyze/expr/local_reference.hpp>
#include <jank/analyze/expr/var_deref.hpp>
#include <jank/analyze/expr/vector.hpp>
#include <jank/analyze/expr/set.hpp>
#include <jank/analyze/expr/do.hpp>
#include <jank/analyze/expr/let.hpp>
#include <jank/analyze/expr/if.hpp>
#include <jank/analyze/expr/function.hpp>
#include <jank/util/escape.hpp>
#include <jank/util/fmt/print.hpp>

namespace jank::codegen
{
  using namespace jank::analyze;

  wasm_patch_processor::wasm_patch_processor(analyze::expr::def_ref const &def_expr,
                                             jtl::immutable_string const &ns_name,
                                             size_t patch_id)
    : def_expr_{ def_expr }
    , ns_name_{ ns_name }
    , patch_id_{ patch_id }
  {
  }

  jtl::immutable_string wasm_patch_processor::fresh_local()
  {
    return jtl::immutable_string{ "tmp_" } + std::to_string(local_counter_++);
  }

  jtl::immutable_string wasm_patch_processor::munge_name(jtl::immutable_string const &name)
  {
    /* Convert jank name to valid C identifier.
     * Replace hyphens with underscores, etc. */
    jtl::string_builder result;
    for(auto const c : name)
    {
      if(c == '-' || c == '.' || c == '/' || c == '?' || c == '!' || c == '*' || c == '+')
      {
        result('_');
      }
      else
      {
        result(c);
      }
    }
    return result.release();
  }

  void wasm_patch_processor::gen_imports()
  {
    output_(R"(#include <stdint.h>

extern "C" {

// Import runtime helpers from main module
extern void *jank_box_integer(int64_t value);
extern int64_t jank_unbox_integer(void *obj);
extern void *jank_box_double(double value);
extern double jank_unbox_double(void *obj);
extern void *jank_make_string(const char *str);
extern void *jank_call_var(const char *ns, const char *name, int argc, void **args);
extern void *jank_deref_var(const char *ns, const char *name);
extern void *jank_make_keyword(const char *ns, const char *name);
extern void *jank_make_symbol(const char *ns, const char *name);
extern void *jank_make_vector(int argc, void **elements);
extern void *jank_make_set(int argc, void **elements);
extern void *jank_make_list(int argc, void **elements);
extern void *jank_make_map(int argc, void **elements);
extern void *jank_println(int argc, void **args);
extern void *jank_nil_value();
extern int jank_truthy(void *obj);
extern void *jank_make_fn_wrapper(void *fn_ptr, int arity);

)");
  }

  void wasm_patch_processor::gen_patch_symbol_struct()
  {
    output_(R"(// Patch symbol metadata
struct patch_symbol {
  const char *qualified_name;
  const char *signature;
  void *fn_ptr;
};

)");
  }

  jtl::immutable_string wasm_patch_processor::gen_expr(analyze::expression_ref const &expr)
  {
    return visit_expr(
      [this](auto const &typed_expr) -> jtl::immutable_string {
        using T = std::decay_t<decltype(*typed_expr)>;

        if constexpr(std::same_as<T, expr::primitive_literal>)
        {
          return gen_primitive_literal(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::call>)
        {
          return gen_call(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::local_reference>)
        {
          return gen_local_reference(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::var_deref>)
        {
          return gen_var_deref(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::vector>)
        {
          return gen_vector(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::set>)
        {
          return gen_set(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::do_>)
        {
          return gen_do(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::let>)
        {
          return gen_let(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::if_>)
        {
          return gen_if(typed_expr);
        }
        else if constexpr(std::same_as<T, expr::function>)
        {
          return gen_function(typed_expr);
        }
        else
        {
          /* Unsupported expression type - return nil for now */
          auto const tmp{ fresh_local() };
          util::format_to(*current_output_,
                          "  void *{} = jank_nil_value(); // unsupported expr\n",
                          tmp);
          return tmp;
        }
      },
      expr);
  }

  jtl::immutable_string
  wasm_patch_processor::gen_primitive_literal(analyze::expr::primitive_literal_ref const &expr)
  {
    auto const tmp{ fresh_local() };

    runtime::visit_object(
      [&](auto const typed_o) {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(std::same_as<T, runtime::obj::nil>)
        {
          util::format_to(*current_output_, "  void *{} = jank_nil_value();\n", tmp);
        }
        else if constexpr(std::same_as<T, runtime::obj::boolean>)
        {
          if(typed_o->data)
          {
            util::format_to(*current_output_, "  void *{} = jank_box_integer(1); // true\n", tmp);
          }
          else
          {
            util::format_to(*current_output_, "  void *{} = jank_nil_value(); // false\n", tmp);
          }
        }
        else if constexpr(std::same_as<T, runtime::obj::integer>)
        {
          util::format_to(*current_output_,
                          "  void *{} = jank_box_integer({});\n",
                          tmp,
                          typed_o->data);
        }
        else if constexpr(std::same_as<T, runtime::obj::real>)
        {
          if(std::isinf(typed_o->data))
          {
            if(typed_o->data > 0)
            {
              util::format_to(*current_output_, "  void *{} = jank_box_double(INFINITY);\n", tmp);
            }
            else
            {
              util::format_to(*current_output_, "  void *{} = jank_box_double(-INFINITY);\n", tmp);
            }
          }
          else if(std::isnan(typed_o->data))
          {
            util::format_to(*current_output_, "  void *{} = jank_box_double(NAN);\n", tmp);
          }
          else
          {
            util::format_to(*current_output_,
                            "  void *{} = jank_box_double({});\n",
                            tmp,
                            typed_o->data);
          }
        }
        else if constexpr(std::same_as<T, runtime::obj::keyword>)
        {
          util::format_to(*current_output_,
                          "  void *{} = jank_make_keyword(\"{}\", \"{}\");\n",
                          tmp,
                          typed_o->sym->ns,
                          typed_o->sym->name);
        }
        else if constexpr(std::same_as<T, runtime::obj::symbol>)
        {
          util::format_to(*current_output_,
                          "  void *{} = jank_make_symbol(\"{}\", \"{}\");\n",
                          tmp,
                          typed_o->ns,
                          typed_o->name);
        }
        else if constexpr(std::same_as<T, runtime::obj::persistent_string>)
        {
          util::format_to(*current_output_,
                          "  void *{} = jank_make_string(\"{}\");\n",
                          tmp,
                          util::escape(typed_o->data));
        }
        else
        {
          /* Fall back to nil for unsupported literals */
          util::format_to(*current_output_,
                          "  void *{} = jank_nil_value(); // unsupported literal\n",
                          tmp);
        }
      },
      expr->data);

    return tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_call(analyze::expr::call_ref const &expr)
  {
    /* First, generate code for all arguments */
    native_vector<jtl::immutable_string> arg_tmps;
    for(auto const &arg : expr->arg_exprs)
    {
      arg_tmps.push_back(gen_expr(arg));
    }

    /* Generate the call based on source type */
    auto const tmp{ fresh_local() };

    /* Check if source is a var_deref - this is the common case */
    if(expr->source_expr->kind == expression_kind::var_deref)
    {
      auto const var_deref{ jtl::static_ref_cast<expr::var_deref>(expr->source_expr) };
      auto const ns_str{ var_deref->qualified_name->ns };
      auto const name_str{ var_deref->qualified_name->name };

      /* Generate args array */
      if(arg_tmps.empty())
      {
        util::format_to(*current_output_,
                        "  void *{} = jank_call_var(\"{}\", \"{}\", 0, nullptr);\n",
                        tmp,
                        ns_str,
                        name_str);
      }
      else
      {
        auto const args_array{ fresh_local() };
        util::format_to(*current_output_, "  void *{}[] = ", args_array);
        (*current_output_)("{ ");
        for(size_t i = 0; i < arg_tmps.size(); ++i)
        {
          if(i > 0)
          {
            (*current_output_)(", ");
          }
          (*current_output_)(arg_tmps[i]);
        }
        (*current_output_)(" };\n");

        util::format_to(*current_output_,
                        "  void *{} = jank_call_var(\"{}\", \"{}\", {}, {});\n",
                        tmp,
                        ns_str,
                        name_str,
                        arg_tmps.size(),
                        args_array);
      }
    }
    else
    {
      /* For other callable sources, generate the source and call it */
      auto const source_tmp{ gen_expr(expr->source_expr) };
      /* TODO: Implement generic callable invocation */
      util::format_to(*current_output_,
                      "  void *{} = jank_nil_value(); // TODO: call non-var {}\n",
                      tmp,
                      source_tmp);
    }

    return tmp;
  }

  jtl::immutable_string
  wasm_patch_processor::gen_local_reference(analyze::expr::local_reference_ref const &expr)
  {
    /* Local references are just variable names - they should already be in scope */
    return munge_name(expr->name->name);
  }

  jtl::immutable_string
  wasm_patch_processor::gen_var_deref(analyze::expr::var_deref_ref const &expr)
  {
    auto const tmp{ fresh_local() };
    util::format_to(*current_output_,
                    "  void *{} = jank_deref_var(\"{}\", \"{}\");\n",
                    tmp,
                    expr->qualified_name->ns,
                    expr->qualified_name->name);
    return tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_vector(analyze::expr::vector_ref const &expr)
  {
    native_vector<jtl::immutable_string> elem_tmps;
    for(auto const &elem : expr->data_exprs)
    {
      elem_tmps.push_back(gen_expr(elem));
    }

    auto const tmp{ fresh_local() };

    if(elem_tmps.empty())
    {
      util::format_to(*current_output_, "  void *{} = jank_make_vector(0, nullptr);\n", tmp);
    }
    else
    {
      auto const elems_array{ fresh_local() };
      util::format_to(*current_output_, "  void *{}[] = ", elems_array);
      (*current_output_)("{ ");
      for(size_t i = 0; i < elem_tmps.size(); ++i)
      {
        if(i > 0)
        {
          (*current_output_)(", ");
        }
        (*current_output_)(elem_tmps[i]);
      }
      (*current_output_)(" };\n");

      util::format_to(*current_output_,
                      "  void *{} = jank_make_vector({}, {});\n",
                      tmp,
                      elem_tmps.size(),
                      elems_array);
    }

    return tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_set(analyze::expr::set_ref const &expr)
  {
    native_vector<jtl::immutable_string> elem_tmps;
    for(auto const &elem : expr->data_exprs)
    {
      elem_tmps.push_back(gen_expr(elem));
    }

    auto const tmp{ fresh_local() };

    if(elem_tmps.empty())
    {
      util::format_to(*current_output_, "  void *{} = jank_make_set(0, nullptr);\n", tmp);
    }
    else
    {
      auto const elems_array{ fresh_local() };
      util::format_to(*current_output_, "  void *{}[] = ", elems_array);
      (*current_output_)("{ ");
      for(size_t i = 0; i < elem_tmps.size(); ++i)
      {
        if(i > 0)
        {
          (*current_output_)(", ");
        }
        (*current_output_)(elem_tmps[i]);
      }
      (*current_output_)(" };\n");

      util::format_to(*current_output_,
                      "  void *{} = jank_make_set({}, {});\n",
                      tmp,
                      elem_tmps.size(),
                      elems_array);
    }

    return tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_do(analyze::expr::do_ref const &expr)
  {
    if(expr->values.empty())
    {
      auto const tmp{ fresh_local() };
      util::format_to(*current_output_, "  void *{} = jank_nil_value();\n", tmp);
      return tmp;
    }

    /* Generate all expressions, return the last one */
    jtl::immutable_string last_tmp;
    for(auto const &val : expr->values)
    {
      last_tmp = gen_expr(val);
    }
    return last_tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_let(analyze::expr::let_ref const &expr)
  {
    /* Generate bindings */
    for(auto const &[name, init_expr] : expr->pairs)
    {
      auto const init_tmp{ gen_expr(init_expr) };
      auto const local_name{ munge_name(name->name) };
      util::format_to(*current_output_, "  void *{} = {};\n", local_name, init_tmp);
    }

    /* Generate body */
    return gen_do(expr->body);
  }

  jtl::immutable_string wasm_patch_processor::gen_if(analyze::expr::if_ref const &expr)
  {
    auto const cond_tmp{ gen_expr(expr->condition) };
    auto const result_tmp{ fresh_local() };

    util::format_to(*current_output_, "  void *{};\n", result_tmp);
    util::format_to(*current_output_, "  if(jank_truthy({})) {{\n", cond_tmp);

    auto const then_tmp{ gen_expr(expr->then) };
    util::format_to(*current_output_, "    {} = {};\n", result_tmp, then_tmp);

    (*current_output_)("  } else {\n");

    if(expr->else_.is_some())
    {
      auto const else_tmp{ gen_expr(expr->else_.unwrap()) };
      util::format_to(*current_output_, "    {} = {};\n", result_tmp, else_tmp);
    }
    else
    {
      util::format_to(*current_output_, "    {} = jank_nil_value();\n", result_tmp);
    }

    (*current_output_)("  }\n");

    return result_tmp;
  }

  jtl::immutable_string wasm_patch_processor::gen_function(analyze::expr::function_ref const &expr)
  {
    /* Generate an anonymous function and return a wrapper */
    auto const anon_id{ anon_fn_counter_++ };
    auto const fn_name{ jtl::immutable_string{ "jank_anon_" } + std::to_string(patch_id_) + "_"
                        + std::to_string(anon_id) };

    /* For now, support single-arity functions only */
    if(expr->arities.empty())
    {
      auto const tmp{ fresh_local() };
      util::format_to(*current_output_, "  void *{} = jank_nil_value(); // empty fn\n", tmp);
      return tmp;
    }

    auto const &arity{ expr->arities[0] };
    auto const param_count{ arity.params.size() };

    /* Generate the anonymous function declaration into anon_fns_ buffer */
    util::format_to(anon_fns_, "__attribute__((visibility(\"default\")))\nvoid *{}(", fn_name);
    for(size_t i = 0; i < param_count; ++i)
    {
      if(i > 0)
      {
        anon_fns_(", ");
      }
      util::format_to(anon_fns_, "void *{}", munge_name(arity.params[i]->name));
    }
    anon_fns_(") {\n");

    /* Generate body directly into anon_fns_ by temporarily swapping the output pointer */
    auto *saved_output{ current_output_ };
    current_output_ = &anon_fns_;

    /* Generate the body */
    auto const body_tmp{ gen_do(arity.body) };
    util::format_to(*current_output_, "  return {};\n", body_tmp);

    /* Restore output pointer */
    current_output_ = saved_output;
    anon_fns_("}\n\n");

    /* Return a wrapper for the anonymous function */
    auto const tmp{ fresh_local() };
    util::format_to(output_,
                    "  void *{} = jank_make_fn_wrapper((void *){}, {});\n",
                    tmp,
                    fn_name,
                    param_count);
    return tmp;
  }

  jtl::immutable_string wasm_patch_processor::generate()
  {
    /* The def_expr should contain a function value */
    if(!def_expr_->value.is_some())
    {
      return "// ERROR: def has no value\n";
    }

    auto const &value_expr{ def_expr_->value.unwrap() };
    if(value_expr->kind != expression_kind::function)
    {
      return "// ERROR: def value is not a function\n";
    }

    auto const fn_expr{ jtl::static_ref_cast<expr::function>(value_expr) };

    /* Get the var name from the def.
     * Always use ns_name_ since the caller has already applied the correct
     * precedence (explicit ns param > def's symbol > session's current ns). */
    auto const var_ns{ ns_name_ };
    auto const var_name{ def_expr_->name->name };

    /* For now, support single-arity functions only */
    if(fn_expr->arities.empty())
    {
      return "// ERROR: function has no arities\n";
    }

    auto const &arity{ fn_expr->arities[0] };
    auto const param_count{ arity.params.size() };

    /* Generate the main function name */
    auto const fn_c_name{ jtl::immutable_string{ "jank_" } + munge_name(var_ns) + "_"
                          + munge_name(var_name) + "_" + std::to_string(patch_id_) };

    /* Generate the function body into output_ buffer */
    auto const body_tmp{ gen_do(arity.body) };

    /* Build the final output */
    jtl::string_builder final_output;
    final_output("// Auto-generated WASM hot-reload patch\n");
    util::format_to(final_output, "// Patch ID: {}\n\n", patch_id_);

    /* Add imports */
    final_output(R"(#include <stdint.h>

extern "C" {

// Import runtime helpers from main module
extern void *jank_box_integer(int64_t value);
extern int64_t jank_unbox_integer(void *obj);
extern void *jank_box_double(double value);
extern double jank_unbox_double(void *obj);
extern void *jank_make_string(const char *str);
extern void *jank_call_var(const char *ns, const char *name, int argc, void **args);
extern void *jank_deref_var(const char *ns, const char *name);
extern void *jank_make_keyword(const char *ns, const char *name);
extern void *jank_make_symbol(const char *ns, const char *name);
extern void *jank_make_vector(int argc, void **elements);
extern void *jank_make_set(int argc, void **elements);
extern void *jank_make_list(int argc, void **elements);
extern void *jank_make_map(int argc, void **elements);
extern void *jank_println(int argc, void **args);
extern void *jank_nil_value();
extern int jank_truthy(void *obj);
extern void *jank_make_fn_wrapper(void *fn_ptr, int arity);

// Patch symbol metadata
struct patch_symbol {
  const char *qualified_name;
  const char *signature;
  void *fn_ptr;
};

)");

    /* Add anonymous functions */
    final_output(anon_fns_.view());

    /* Add main function signature */
    util::format_to(final_output, "__attribute__((visibility(\"default\")))\nvoid *{}(", fn_c_name);
    for(size_t i = 0; i < param_count; ++i)
    {
      if(i > 0)
      {
        final_output(", ");
      }
      util::format_to(final_output, "void *{}", munge_name(arity.params[i]->name));
    }
    final_output(") {\n");

    /* Add function body (from output_ buffer) */
    final_output(output_.view());
    util::format_to(final_output, "  return {};\n", body_tmp);
    final_output("}\n\n");

    /* Add patch metadata export */
    util::format_to(final_output,
                    "// Patch metadata export - UNIQUE NAME with patch ID {}\n",
                    patch_id_);
    final_output("__attribute__((visibility(\"default\")))\n");
    util::format_to(final_output, "patch_symbol *jank_patch_symbols_{}(int *count) ", patch_id_);
    final_output("{\n  static patch_symbol symbols[] = {\n    { ");
    util::format_to(final_output,
                    R"("{}/{}", "{}", (void *){})",
                    var_ns,
                    var_name,
                    param_count,
                    fn_c_name);
    final_output(" }\n  };\n  *count = 1;\n  return symbols;\n}\n\n}\n");

    return final_output.release();
  }
} // namespace jank::codegen
