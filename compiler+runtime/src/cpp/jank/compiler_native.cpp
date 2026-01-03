#include <string>
#include <string_view>

#include <jank/compiler_native.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/persistent_list.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/aot/processor.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/analyze/pass/optimize.hpp>
#include <jank/evaluate.hpp>
#include <jank/codegen/llvm_processor.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/util/cli.hpp>
#include <jank/util/clang_format.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/compile_server/remote_compile.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/raw_ostream.h>

namespace jank::compiler_native
{
  using namespace jank;
  using namespace jank::runtime;

  template <typename Fn>
  static object_ref with_pipeline(object_ref const form, Fn &&fn)
  {
    /* Get the current namespace from *ns* var. This is needed for proper symbol resolution,
     * especially for C++ interop symbols that are namespace-scoped. */
    auto const ns_obj(__rt_ctx->intern_var("clojure.core", "*ns*").expect_ok()->deref());
    auto const current_ns(expect_object<runtime::ns>(ns_obj));

    /* Set up thread bindings for *ns* so the analyzer can properly resolve symbols.
     * This is essential for C++ interop where symbols like 'sdfx' are aliases defined
     * in the namespace's native requires. Without this binding, the analyzer can't
     * find those aliases. */
    auto const bindings = runtime::obj::persistent_hash_map::create_unique(
      std::make_pair(__rt_ctx->current_ns_var, current_ns));
    runtime::context::binding_scope const scope{ bindings };

    /* We use a clean analyze::processor so we don't share lifted items from other REPL
     * evaluations. */
    analyze::processor an_prc;
    auto const analyzed_expr(an_prc.analyze(form, analyze::expression_position::value).expect_ok());
    auto const optimized_expr(analyze::pass::optimize(analyzed_expr));
    auto const wrapped_expr(evaluate::wrap_expression(optimized_expr, "native_source", {}));
    auto module(current_ns->to_string());

    return fn(analyzed_expr, optimized_expr, wrapped_expr, module);
  }

  static void forward_string(std::string_view const text)
  {
    runtime::forward_output(text);
    runtime::forward_output(std::string_view{ "\n", 1 });
  }

  static object_ref make_string_object(std::string const &text)
  {
    return make_box<obj::persistent_string>(text);
  }

  static object_ref make_string_object(jtl::immutable_string const &text)
  {
    return make_box<obj::persistent_string>(text);
  }

  static std::string render_llvm_ir(analyze::expr::function_ref const &wrapped_expr,
                                    jtl::immutable_string const &module,
                                    bool const optimize)
  {
    codegen::llvm_processor const cg_prc{ wrapped_expr, module, codegen::compilation_target::eval };
    cg_prc.gen().expect_ok();
    if(optimize)
    {
      cg_prc.optimize();
    }
    auto const raw_module(cg_prc.get_module().getModuleUnlocked());
    std::string ir_text;
    llvm::raw_string_ostream ir_stream{ ir_text };
    raw_module->print(ir_stream, nullptr);
    ir_stream.flush();
    return ir_text;
  }

  static jtl::immutable_string
  render_cpp_declaration(analyze::expr::function_ref const &wrapped_expr,
                         jtl::immutable_string const &module)
  {
    codegen::processor cg_prc{ wrapped_expr, module, codegen::compilation_target::eval };
    return cg_prc.declaration_str();
  }

  static jtl::immutable_string
  render_cpp_aot_declaration(analyze::expr::function_ref const &wrapped_expr,
                             jtl::immutable_string const &module)
  {
    codegen::processor cg_prc{ wrapped_expr, module, codegen::compilation_target::module };
    return cg_prc.declaration_str();
  }

  static jtl::immutable_string
  render_cpp_wasm_aot_declaration(analyze::expr::function_ref const &wrapped_expr,
                                  jtl::immutable_string const &module)
  {
    codegen::processor cg_prc{ wrapped_expr, module, codegen::compilation_target::wasm_aot };
    return cg_prc.declaration_str();
  }

  static object_ref native_source(object_ref const form)
  {
#ifdef JANK_IOS_JIT
    /* On iOS JIT mode with remote compilation enabled, delegate to the compile server.
     * This is necessary because local analysis can't resolve C++ interop symbols
     * (headers aren't loaded on iOS). */
    if(compile_server::is_remote_compile_enabled())
    {
      auto const code = runtime::to_code_string(form);
      auto const current_ns = __rt_ctx->current_ns();
      auto const ns_str = std::string(current_ns->name->name.data(), current_ns->name->name.size());

      auto const response = compile_server::remote_native_source(code, ns_str);
      if(response.success)
      {
        auto formatted(util::format_cpp_source(response.source).expect_ok());
        forward_string(std::string_view{ formatted.data(), formatted.size() });
      }
      else
      {
        throw std::runtime_error("Remote native-source failed: " + response.error);
      }
      return jank_nil();
    }
#endif

    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        if(util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
        {
          auto const ir_text(render_llvm_ir(wrapped_expr, module, true));
          forward_string(std::string_view{ ir_text });
        }
        else
        {
          auto const declaration(render_cpp_declaration(wrapped_expr, module));
          auto formatted(util::format_cpp_source(declaration).expect_ok());
          forward_string(std::string_view{ formatted.data(), formatted.size() });
        }

        return jank_nil();
      });
  }

  static object_ref native_cpp_source(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        auto const declaration(render_cpp_declaration(wrapped_expr, module));
        auto formatted(util::format_cpp_source(declaration).expect_ok());
        forward_string(std::string_view{ formatted.data(), formatted.size() });
        return jank_nil();
      });
  }

  static object_ref native_analyzed_form(object_ref const form)
  {
    return with_pipeline(form,
                         [](auto const &analyzed_expr, auto const &, auto const &, auto const &) {
                           return analyzed_expr->to_runtime_data();
                         });
  }

  static object_ref native_optimized_form(object_ref const form)
  {
    return with_pipeline(form,
                         [](auto const &, auto const &optimized_expr, auto const &, auto const &) {
                           return optimized_expr->to_runtime_data();
                         });
  }

  static object_ref native_wrapped_form(object_ref const form)
  {
    return with_pipeline(form,
                         [](auto const &, auto const &, auto const &wrapped_expr, auto const &) {
                           return wrapped_expr->to_runtime_data();
                         });
  }

  static object_ref native_llvm_ir(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        return make_string_object(render_llvm_ir(wrapped_expr, module, false));
      });
  }

  static object_ref native_llvm_ir_optimized(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        return make_string_object(render_llvm_ir(wrapped_expr, module, true));
      });
  }

  static object_ref native_cpp_source_raw(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        return make_string_object(render_cpp_declaration(wrapped_expr, module));
      });
  }

  static object_ref native_cpp_source_formatted(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        auto const declaration(render_cpp_declaration(wrapped_expr, module));
        auto formatted(util::format_cpp_source(declaration).expect_ok());
        return make_string_object(formatted);
      });
  }

  static object_ref native_aot_source_raw(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        return make_string_object(render_cpp_aot_declaration(wrapped_expr, module));
      });
  }

  static object_ref native_aot_source_formatted(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        auto const declaration(render_cpp_aot_declaration(wrapped_expr, module));
        auto formatted(util::format_cpp_source(declaration).expect_ok());
        return make_string_object(formatted);
      });
  }

  static object_ref native_wasm_aot_source_raw(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        return make_string_object(render_cpp_wasm_aot_declaration(wrapped_expr, module));
      });
  }

  static object_ref native_wasm_aot_source_formatted(object_ref const form)
  {
    return with_pipeline(
      form,
      [](auto const &, auto const &, auto const &wrapped_expr, auto const &module) {
        auto const declaration(render_cpp_wasm_aot_declaration(wrapped_expr, module));
        auto formatted(util::format_cpp_source(declaration).expect_ok());
        return make_string_object(formatted);
      });
  }

  static object_ref native_aot_entrypoint_source(object_ref const form)
  {
    auto const list(dyn_cast<obj::persistent_list>(form));
    if(list.is_nil() || list->count() != 4)
    {
      return jank_nil();
    }

    auto const second(list->next()->first());
    auto const third(list->next()->next()->first());
    auto const fourth(list->next()->next()->next()->first());

    object_ref var_obj(second);
    auto const l(dyn_cast<obj::persistent_list>(second));
    if(l.is_some())
    {
      if(l->count() == 2)
      {
        auto const op(dyn_cast<obj::symbol>(l->first()));
        if(op.is_some() && op->name == "var")
        {
          auto const sym(dyn_cast<obj::symbol>(l->next()->first()));
          if(sym.is_some())
          {
            auto const ns(sym->ns.empty() ? __rt_ctx->current_ns()->name->name : sym->ns);
            auto const v(__rt_ctx->find_var(ns, sym->name));
            if(v.is_some())
            {
              var_obj = v;
            }
          }
        }
      }
    }

    auto const name_obj(dyn_cast<obj::persistent_string>(third));
    auto const schema_obj(fourth);

    if(var_obj.is_nil() || !dyn_cast<var>(var_obj).is_some() || name_obj.is_nil()
       || schema_obj.is_nil())
    {
      return jank_nil();
    }

    auto const result(aot::generate_entrypoint_source(var_obj, name_obj->data, schema_obj));
    if(result.is_ok())
    {
      auto formatted(util::format_cpp_source(result.expect_ok()).expect_ok());
      forward_string(std::string_view{ formatted.data(), formatted.size() });
    }

    return jank_nil();
  }
}

extern "C" void jank_load_jank_compiler_native()
{
  using namespace jank;
  using namespace jank::runtime;

  auto const ns(__rt_ctx->intern_ns("jank.compiler-native"));

  auto const intern_fn([=](jtl::immutable_string const &name, auto const fn) {
    ns->intern_var(name)->bind_root(
      make_box<obj::native_function_wrapper>(convert_function(fn))
        ->with_meta(obj::persistent_hash_map::create_unique(std::make_pair(
          __rt_ctx->intern_keyword("name").expect_ok(),
          make_box(obj::symbol{ __rt_ctx->current_ns()->to_string(), name }.to_string())))));
  });
  intern_fn("native-source", &compiler_native::native_source);
  intern_fn("native-cpp-source", &compiler_native::native_cpp_source);
  intern_fn("native-analyzed-form", &compiler_native::native_analyzed_form);
  intern_fn("native-optimized-form", &compiler_native::native_optimized_form);
  intern_fn("native-wrapped-form", &compiler_native::native_wrapped_form);
  intern_fn("native-llvm-ir", &compiler_native::native_llvm_ir);
  intern_fn("native-llvm-ir-optimized", &compiler_native::native_llvm_ir_optimized);
  intern_fn("native-cpp-source-raw", &compiler_native::native_cpp_source_raw);
  intern_fn("native-cpp-source-formatted", &compiler_native::native_cpp_source_formatted);
  intern_fn("native-aot-source-raw", &compiler_native::native_aot_source_raw);
  intern_fn("native-aot-source-formatted", &compiler_native::native_aot_source_formatted);
  intern_fn("native-wasm-aot-source-raw", &compiler_native::native_wasm_aot_source_raw);
  intern_fn("native-wasm-aot-source-formatted", &compiler_native::native_wasm_aot_source_formatted);
  intern_fn("native-aot-entrypoint-source", &compiler_native::native_aot_entrypoint_source);
}
