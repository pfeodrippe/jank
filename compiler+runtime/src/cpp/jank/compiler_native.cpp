#include <string_view>

#include <jank/compiler_native.hpp>
#include <jank/runtime/convert/function.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/analyze/pass/optimize.hpp>
#include <jank/evaluate.hpp>
#include <jank/codegen/llvm_processor.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/util/cli.hpp>
#include <jank/util/clang_format.hpp>
#include <jank/util/fmt/print.hpp>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/raw_ostream.h>

namespace jank::compiler_native
{
  using namespace jank;
  using namespace jank::runtime;

  static object_ref
  emit_native_source(object_ref const form, util::cli::codegen_type const target_codegen)
  {
    /* We use a clean analyze::processor so we don't share lifted items from other REPL
     * evaluations. */
    analyze::processor an_prc;
    auto const expr(analyze::pass::optimize(
      an_prc.analyze(form, analyze::expression_position::value).expect_ok()));
    auto const wrapped_expr(evaluate::wrap_expression(expr, "native_source", {}));
    auto const &module(
      expect_object<runtime::ns>(__rt_ctx->intern_var("clojure.core", "*ns*").expect_ok()->deref())
        ->to_string());

    if(target_codegen == util::cli::codegen_type::llvm_ir)
    {
      codegen::llvm_processor const cg_prc{ wrapped_expr,
                                            module,
                                            codegen::compilation_target::eval };
      cg_prc.gen().expect_ok();
      cg_prc.optimize();
      auto const raw_module(cg_prc.get_module().getModuleUnlocked());
      std::string ir_text;
      llvm::raw_string_ostream ir_stream{ ir_text };
      raw_module->print(ir_stream, nullptr);
      ir_stream.flush();
      runtime::forward_output(ir_text);
      runtime::forward_output(std::string_view{ "\n", 1 });
    }
    else
    {
      codegen::processor cg_prc{ wrapped_expr, module, codegen::compilation_target::eval };
      auto formatted(util::format_cpp_source(cg_prc.declaration_str()).expect_ok());
      runtime::forward_output(std::string_view{ formatted.data(), formatted.size() });
      runtime::forward_output(std::string_view{ "\n", 1 });
    }

    return jank_nil;
  }

  static object_ref native_source(object_ref const form)
  {
    return emit_native_source(form, util::cli::opts.codegen);
  }

  static object_ref native_cpp_source(object_ref const form)
  {
    return emit_native_source(form, util::cli::codegen_type::cpp);
  }
}

extern "C" jank_object_ref jank_load_jank_compiler_native()
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

  return jank_nil.erase();
}
