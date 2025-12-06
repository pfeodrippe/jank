#if !defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_HAS_CPPINTEROP)
  #include <Interpreter/Compatibility.h>
  #include <Interpreter/CppInterOpInterpreter.h>
  #include <clang/Interpreter/CppInterOp.h>
  #include <clang/AST/Decl.h>
  #include <clang/AST/DeclCXX.h>
  #include <clang/Frontend/CompilerInstance.h>
  #include <llvm/Support/Casting.h>
#endif

#ifndef JANK_TARGET_WASM
  #include <llvm/ExecutionEngine/Orc/LLJIT.h>
#endif

#include <jank/runtime/context.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/behavior/callable.hpp>
#ifndef JANK_TARGET_WASM
  #include <jank/codegen/llvm_processor.hpp>
#endif
#include <jank/codegen/processor.hpp>
#include <jank/jit/processor.hpp>
#include <jank/evaluate.hpp>
#include <jank/profile/time.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/clang_format.hpp>
#include <jank/analyze/visit.hpp>
#include <jank/analyze/cpp_util.hpp>
#include <jank/error/analyze.hpp>
#include <jank/error/codegen.hpp>

namespace jank::evaluate
{
  using namespace jank::runtime;
  using namespace jank::analyze;

  namespace
  {
    /* Normalizes C++ names by replacing "::" with "." for jank symbol compatibility. */
    std::string normalize_cpp_entity_name(std::string qualified)
    {
      std::string normalized;
      normalized.reserve(qualified.size());
      for(size_t idx{}; idx < qualified.size(); ++idx)
      {
        if(qualified[idx] == ':' && idx + 1 < qualified.size() && qualified[idx + 1] == ':')
        {
          normalized.push_back('.');
          ++idx;
          continue;
        }
        normalized.push_back(qualified[idx]);
      }
      return normalized;
    }
  }

  /* TODO: Move postwalk into the nodes. */
  template <typename T, typename F>
  requires std::is_base_of_v<expression, T>
  static void walk(T const &expr, F const &f)
  {
    if constexpr(std::same_as<T, expr::call>)
    {
      walk(expr.source_expr, f);
      for(auto const &form : expr.arg_exprs)
      {
        walk(form, f);
      }
    }
    else if constexpr(std::same_as<T, expr::def>)
    {
      if(expr.value.is_some())
      {
        walk(expr.value.unwrap(), f);
      }
    }
    else if constexpr(std::same_as<T, expr::if_>)
    {
      walk(expr.condition, f);
      walk(expr.then, f);
      if(expr.else_.is_some())
      {
        walk(expr.else_.unwrap(), f);
      }
    }
    else if constexpr(std::same_as<T, expr::do_>)
    {
      for(auto const &form : expr.values)
      {
        walk(form, f);
      }
    }
    else if constexpr(std::same_as<T, expr::let> || std::same_as<T, expr::letfn>)
    {
      walk(expr.body, f);
    }
    else if constexpr(std::same_as<T, expr::throw_>)
    {
      walk(expr.value, f);
    }
    else if constexpr(std::same_as<T, expr::try_>)
    {
      walk(expr.body, f);
      if(expr.catch_body.is_some())
      {
        walk(expr.catch_body.unwrap().body, f);
      }
      if(expr.finally_body.is_some())
      {
        walk(expr.finally_body.unwrap(), f);
      }
    }
    else if constexpr(std::same_as<T, expr::list>)
    {
      for(auto const &form : expr.data_exprs)
      {
        walk(form, f);
      }
    }
    else if constexpr(std::same_as<T, expr::vector>)
    {
      for(auto const &form : expr.data_exprs)
      {
        walk(form, f);
      }
    }
    else if constexpr(std::same_as<T, expr::set>)
    {
      for(auto const &form : expr.data_exprs)
      {
        walk(form, f);
      }
    }
    else if constexpr(std::same_as<T, expr::map>)
    {
      for(auto const &form : expr.data_exprs)
      {
        walk(form.first, f);
        walk(form.second, f);
      }
    }
    /* TODO: function */

    f(expr);
  }

  template <typename F>
  static void walk(expression_ref const expr, F const &f)
  {
    visit_expr([&](auto const typed_expr) { walk(*typed_expr, f); }, expr);
  }

  template <typename F>
  static void walk(expr::function_arity const &arity, F const &f)
  {
    walk(arity.body, f);
  }

  /* Some expressions don't make sense to eval outright and aren't fns that can be JIT compiled.
   * For those, we wrap them in a fn expression and then JIT compile and call them.
   *
   * There's an oddity here, since that expr wouldn't've been analyzed within a fn frame, so
   * its lifted vars/constants, for example, aren't in a fn frame. Instead, they're put in the
   * root frame. So, when wrapping this expr, we give the fn the root frame, but change its
   * type to a fn frame. */
  template <typename E>
  static expr::function_ref wrap_expression(jtl::ref<E> const orig_expr,
                                            jtl::immutable_string const &name,
                                            native_vector<obj::symbol_ref> params)
  {
    auto ret{ jtl::make_ref<expr::function>() };
    auto expr{ make_ref<E>(orig_expr) };
    ret->kind = analyze::expression_kind::function;
    ret->name = name;
    ret->unique_name = __rt_ctx->unique_namespaced_string(ret->name);
    ret->meta = obj::persistent_hash_map::empty();

    auto const &closest_fn_frame(local_frame::find_closest_fn_frame(*expr->frame));

    auto const frame{ jtl::make_ref<local_frame>(local_frame::frame_type::fn,
                                                 expr->frame->parent) };
    auto const fn_ctx{ jtl::make_ref<expr::function_context>() };
    expr::function_arity arity{ jtl::move(params),
                                jtl::make_ref<expr::do_>(expression_position::tail, frame, true),
                                frame,
                                fn_ctx };
    expr->frame->parent = arity.frame;
    ret->frame = arity.frame->parent.unwrap_or(arity.frame);
    ret->frame->lift_constant(ret->meta);
    fn_ctx->name = ret->name;
    fn_ctx->unique_name = ret->unique_name;
    fn_ctx->fn = ret;
    arity.frame->fn_ctx = fn_ctx;
    arity.fn_ctx = fn_ctx;

    arity.frame->lifted_vars = closest_fn_frame.lifted_vars;
    arity.frame->lifted_constants = closest_fn_frame.lifted_constants;

    arity.fn_ctx->param_count = arity.params.size();
    for(auto const sym : arity.params)
    {
      arity.frame->locals.emplace(sym, local_binding{ sym, sym->name, none, arity.frame });
    }

    auto const expr_type{ cpp_util::expression_type(expr) };
    expression_ref expr_to_add{ expr };
    if(!cpp_util::is_untyped_object(expr_type))
    {
      /* For non-convertible types, use native_print policy. The runtime check for
       * *allow-native-return* happens in the generated code, which allows
       * (binding [*allow-native-return* true] ...) to work at runtime. */
      conversion_policy const policy{ cpp_util::is_trait_convertible(expr_type)
                                        ? conversion_policy::into_object
                                        : conversion_policy::native_print };
      expr_to_add = jtl::make_ref<expr::cpp_cast>(expr->position,
                                                  expr->frame,
                                                  expr->needs_box,
                                                  cpp_util::untyped_object_ptr_type(),
                                                  expr_type,
                                                  policy,
                                                  expr);
      expr->propagate_position(expression_position::value);
    }
    arity.body->values.push_back(expr_to_add);

    walk(arity, [&](auto const &form) {
      using T = std::decay_t<decltype(form)>;

      if constexpr(std::same_as<T, expr::local_reference>)
      {
        auto found_local(expr->frame->find_local_or_capture(form.name));
        if(found_local && !found_local.unwrap().crossed_fns.empty())
        {
          arity.frame->captures[form.name] = *found_local.unwrap().binding;
        }
      }
    });

    ret->arities.emplace_back(jtl::move(arity));

    /* We can't just assign the position here, since we need the position to propagate
     * downward. For example, if this expr is a let, setting its position to tail
     * wouldn't affect the last form of its body, which should also be in tail position.
     *
     * This is what propagation does. */
    ret->arities[0].body->propagate_position(expression_position::tail);

    return ret;
  }

  /* TODO: Expression wrapping makes sense in analyze, not eval. We use it all over the place. */
  expr::function_ref wrap_expressions(native_vector<expression_ref> const &exprs,
                                      processor const &an_prc,
                                      jtl::immutable_string const &name)
  {
    if(exprs.empty())
    {
      return wrap_expression(jtl::make_ref<expr::primitive_literal>(expression_position::tail,
                                                                    an_prc.root_frame,
                                                                    true,
                                                                    jank_nil),
                             name,
                             {});
    }
    else
    {
      /* We'll cheat a little and build a fn using just the first expression. Then we can just
       * add the rest. I'd rather do this than duplicate all of the wrapping logic. */
      auto ret(wrap_expression(exprs[0], name, {}));
      auto &body(ret->arities[0].body->values);
      /* We normally wrap one expression, which is a return statement, but we'll be potentially
       * adding more, so let's not make assumptions yet. */
      body[0]->propagate_position(expression_position::statement);

      /* We normally wrap one expression, which is a return statement, but we'll be potentially
       * adding more, so let's not make assumptions yet. */
      for(auto it{ exprs.begin() + 1 }; it != exprs.end(); ++it)
      {
        auto const expr{ *it };
        expr->propagate_position(expression_position::statement);
        body.emplace_back(expr);
      }

      /* Finally, mark the last body item as our return. */
      body.back()->propagate_position(expression_position::tail);

      return ret;
    }
  }

  expr::function_ref wrap_expression(expression_ref const expr,
                                     jtl::immutable_string const &name,
                                     native_vector<obj::symbol_ref> params)
  {
    return visit_expr(
      [&](auto const typed_expr) { return wrap_expression(typed_expr, name, jtl::move(params)); },
      expr);
  }

  object_ref eval(expression_ref const ex)
  {
    profile::timer const timer{ "eval ast node" };
    object_ref ret{};
    visit_expr([&ret](auto const typed_ex) { ret = eval(typed_ex); }, ex);
    return ret;
  }

  object_ref eval(expr::def_ref const expr)
  {
    auto var(__rt_ctx->intern_var(expr->name).expect_ok());
    var->meta = expr->name->meta;

    auto const meta(var->meta.unwrap_or(jank_nil));
    auto const dynamic(get(meta, __rt_ctx->intern_keyword("dynamic").expect_ok()));
    var->set_dynamic(truthy(dynamic));

    if(expr->value.is_none())
    {
      return var;
    }

    auto const evaluated_value(eval(expr->value.unwrap()));
    var->bind_root(evaluated_value);

    return var;
  }

  object_ref eval(expr::var_deref_ref const expr)
  {
    auto const var(__rt_ctx->find_var(expr->qualified_name));
    return var->deref();
  }

  object_ref eval(expr::var_ref_ref const expr)
  {
    auto const var(__rt_ctx->find_var(expr->qualified_name));
    return var;
  }

  object_ref eval(expr::call_ref const expr)
  {
    auto source(eval(expr->source_expr));
    while(source->type == object_type::var)
    {
      source = deref(source);
    }

    try
    {
      return visit_object(
        [&](auto const typed_source) -> object_ref {
          using T = typename decltype(typed_source)::value_type;

          if constexpr(std::is_base_of_v<behavior::callable, T>)
          {
            native_vector<object_ref> arg_vals;
            arg_vals.reserve(expr->arg_exprs.size());
            for(auto const &arg_expr : expr->arg_exprs)
            {
              arg_vals.emplace_back(eval(arg_expr));
            }

            switch(arg_vals.size())
            {
              case 0:
                return dynamic_call(source);
              case 1:
                return dynamic_call(source, arg_vals[0]);
              case 2:
                return dynamic_call(source, arg_vals[0], arg_vals[1]);
              case 3:
                return dynamic_call(source, arg_vals[0], arg_vals[1], arg_vals[2]);
              case 4:
                return dynamic_call(source, arg_vals[0], arg_vals[1], arg_vals[2], arg_vals[3]);
              case 5:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4]);
              case 6:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5]);
              case 7:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5],
                                    arg_vals[6]);
              case 8:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5],
                                    arg_vals[6],
                                    arg_vals[7]);
              case 9:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5],
                                    arg_vals[6],
                                    arg_vals[7],
                                    arg_vals[8]);
              case 10:
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5],
                                    arg_vals[6],
                                    arg_vals[7],
                                    arg_vals[8],
                                    arg_vals[9]);
              default:
                {
                  return dynamic_call(source,
                                      arg_vals[0],
                                      arg_vals[1],
                                      arg_vals[2],
                                      arg_vals[3],
                                      arg_vals[4],
                                      arg_vals[5],
                                      arg_vals[6],
                                      arg_vals[7],
                                      arg_vals[8],
                                      arg_vals[9],
                                      try_object<obj::persistent_list>(arg_vals[10]));
                }
            }
          }
          else if constexpr(std::same_as<T, obj::persistent_hash_set>
                            || std::same_as<T, obj::persistent_vector>
                            || std::same_as<T, obj::transient_vector>)
          {
            auto const s(expr->arg_exprs.size());
            if(s != 1)
            {
              throw std::runtime_error{
                util::format("Invalid call with {} args to: {}", s, typed_source->to_string())
              };
            }
            return typed_source->call(eval(expr->arg_exprs[0]));
          }
          else if constexpr(std::same_as<T, obj::keyword>
                            || std::same_as<T, obj::persistent_hash_map>
                            || std::same_as<T, obj::persistent_array_map>
                            || std::same_as<T, obj::transient_hash_set>)
          {
            auto const s(expr->arg_exprs.size());
            switch(s)
            {
              case 1:
                return typed_source->call(eval(expr->arg_exprs[0]));
              case 2:
                return typed_source->call(eval(expr->arg_exprs[0]), eval(expr->arg_exprs[1]));
              default:
                throw std::runtime_error{
                  util::format("Invalid call with {} args to: {}", s, typed_source->to_string())
                };
            }
          }
          else
          {
            throw std::runtime_error{ util::format("Invalid call with {} args to: {}",
                                                   expr->arg_exprs.size(),
                                                   typed_source->to_string()) };
          }
        },
        source);
    }
    catch(error_ref const e)
    {
      /* We keep the original form from the call expression so we can point
       * back to it if an exception is thrown during eval. */
      e->add_usage(object_source(expr->form));
      throw e;
    }
  }

  object_ref eval(expr::primitive_literal_ref const expr)
  {
    if(expr->data->type == object_type::keyword)
    {
      auto const d(expect_object<obj::keyword>(expr->data));
      return __rt_ctx->intern_keyword(d->sym->ns, d->sym->name).expect_ok();
    }
    return expr->data;
  }

  object_ref eval(expr::list_ref const expr)
  {
    native_vector<object_ref> ret;
    for(auto const &e : expr->data_exprs)
    {
      ret.emplace_back(eval(e));
    }

    runtime::detail::native_persistent_list const npl{ ret.rbegin(), ret.rend() };
    if(expr->meta.is_some())
    {
      return make_box<obj::persistent_list>(expr->meta.unwrap(), jtl::move(npl));
    }
    else
    {
      return make_box<obj::persistent_list>(jtl::move(npl));
    }
  }

  object_ref eval(expr::vector_ref const expr)
  {
    runtime::detail::native_transient_vector ret;
    for(auto const &e : expr->data_exprs)
    {
      ret.push_back(eval(e));
    }
    if(expr->meta.is_some())
    {
      return make_box<obj::persistent_vector>(expr->meta.unwrap(), ret.persistent());
    }
    else
    {
      return make_box<obj::persistent_vector>(ret.persistent());
    }
  }

  object_ref eval(expr::map_ref const expr)
  {
    auto const size(expr->data_exprs.size());
    if(size <= obj::persistent_array_map::max_size)
    {
      auto const array_box(
        make_array_box<object_ref>(static_cast<usize>(size) * static_cast<usize>(2)));
      usize i{};
      for(auto const &e : expr->data_exprs)
      {
        array_box.data[i++] = eval(e.first);
        array_box.data[i++] = eval(e.second);
      }

      if(expr->meta.is_some())
      {
        return make_box<obj::persistent_array_map>(expr->meta.unwrap(),
                                                   runtime::detail::in_place_unique{},
                                                   array_box,
                                                   size * 2);
      }
      else
      {
        return make_box<obj::persistent_array_map>(runtime::detail::in_place_unique{},
                                                   array_box,
                                                   size * 2);
      }
    }
    else
    {
      runtime::detail::native_transient_hash_map trans;
      for(auto const &e : expr->data_exprs)
      {
        trans.insert({ eval(e.first), eval(e.second) });
      }

      if(expr->meta.is_some())
      {
        return make_box<obj::persistent_hash_map>(expr->meta.unwrap(), trans.persistent());
      }
      else
      {
        return make_box<obj::persistent_hash_map>(trans.persistent());
      }
    }
  }

  object_ref eval(expr::set_ref const expr)
  {
    runtime::detail::native_transient_hash_set ret;
    for(auto const &e : expr->data_exprs)
    {
      ret.insert(eval(e));
    }
    if(expr->meta.is_some())
    {
      return make_box<obj::persistent_hash_set>(expr->meta.unwrap(), jtl::move(ret).persistent());
    }
    else
    {
      return make_box<obj::persistent_hash_set>(jtl::move(ret).persistent());
    }
  }

  object_ref eval(expr::local_reference_ref const)
  /* Doesn't make sense to eval these, since let is wrapped in a fn and JIT compiled. */
  {
    throw make_box("unsupported eval: local_reference").erase();
  }

  object_ref eval(expr::function_ref const expr)
  {
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    auto const &module(
      module::nest_module(expect_object<ns>(__rt_ctx->current_ns_var->deref())->to_string(),
                          munge(expr->unique_name)));

  #ifndef JANK_TARGET_WASM
    /* Native builds support both LLVM IR and C++ codegen */
    if(util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
    {
      /* TODO: Remove extra wrapper, if possible. Just create function object directly? */
      auto const wrapped_expr(wrap_expression(expr, "repl_fn", {}));

      codegen::llvm_processor const cg_prc{ wrapped_expr,
                                            module,
                                            codegen::compilation_target::eval };
      cg_prc.gen().expect_ok();
      cg_prc.optimize();

      __rt_ctx->jit_prc.load_ir_module(jtl::move(cg_prc.get_module()));

      auto const fn(
        __rt_ctx->jit_prc.find_symbol(util::format("{}_0", munge(cg_prc.get_root_fn_name())))
          .expect_ok());
      return reinterpret_cast<object *(*)()>(fn)();
    }
  #else
    /* WASM only supports C++ codegen, not LLVM IR */
    if(util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
    {
      throw make_box("LLVM IR codegen not supported in WASM - use C++ codegen").erase();
    }
  #endif

    /* C++ codegen path - works for both native and WASM */
    codegen::processor cg_prc{ expr, module, codegen::compilation_target::eval };

    /* TODO: Rename to something generic which makes sense for IR and C++ gen? */
    jtl::immutable_string_view const print_settings{ getenv("JANK_PRINT_IR") ?: "" };
    if(print_settings == "1")
    {
      util::println("{}\n", util::format_cpp_source(cg_prc.declaration_str()).expect_ok());
    }

    __rt_ctx->jit_prc.eval_string(cg_prc.declaration_str());
    auto const expr_str{ cg_prc.expression_str() + ".erase()" };
    clang::Value v;
    auto res(
      __rt_ctx->jit_prc.interpreter->ParseAndExecute({ expr_str.data(), expr_str.size() }, &v));
    if(res)
    {
      /* TODO: Helper to turn an llvm::Error into a string. */
      jtl::immutable_string const msg{ "Unable to compile/eval C++ source." };
      llvm::logAllUnhandledErrors(jtl::move(res), llvm::errs(), "error: ");
      throw error::internal_codegen_failure(msg);
    }
    return try_object<obj::jit_function>(v.convertTo<runtime::object *>());
#else /* No CppInterOp */
    (void)expr;
    throw make_box("eval not supported in WASM without CppInterOp").erase();
#endif /* CppInterOp available */
  }

  object_ref eval(expr::recur_ref const)
  /* This will always be in a fn or loop, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: recur").erase();
  }

  object_ref eval(expr::recursion_reference_ref const)
  /* This will always be in a fn, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: recursion_reference").erase();
  }

  object_ref eval(expr::named_recursion_ref const)
  /* This will always be in a fn, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: named_recursion").erase();
  }

  object_ref eval(expr::do_ref const expr)
  {
    object_ref ret{ jank_nil };
    for(auto const &form : expr->values)
    {
      ret = eval(form);
    }
    return ret;
  }

  object_ref eval(expr::let_ref const expr)
  {
    return dynamic_call(eval(wrap_expression(expr, "let", {})));
  }

  object_ref eval(expr::letfn_ref const expr)
  {
    return dynamic_call(eval(wrap_expression(expr, "letfn", {})));
  }

  object_ref eval(expr::if_ref const expr)
  {
    auto const condition(eval(expr->condition));
    if(truthy(condition))
    {
      return eval(expr->then);
    }
    else if(expr->else_.is_some())
    {
      return eval(expr->else_.unwrap());
    }
    return jank_nil;
  }

  object_ref eval(expr::throw_ref const expr)
  {
    /* XXX: Clojure wraps throw expressions. I _suspect_ it does this because
     * clojure.main uses the stack trace to provide source info by stripping out
     * Clojure frames until the first non-Clojure frame is found. If we throw
     * from an eval, maybe that doesn't happen? For now, we support eval, however. */
    throw eval(expr->value);
  }

  object_ref eval(expr::try_ref const expr)
  {
    util::scope_exit const finally{ [=]() {
      if(expr->finally_body)
      {
        eval(expr->finally_body.unwrap());
      }
    } };

    if(!expr->catch_body)
    {
      return eval(expr->body);
    }
    try
    {
      return eval(expr->body);
    }
    catch(object_ref const e)
    {
      return dynamic_call(eval(wrap_expression(expr->catch_body.unwrap().body,
                                               "catch",
                                               { expr->catch_body.unwrap().sym })),
                          e);
    }
  }

  object_ref eval(expr::case_ref const expr)
  {
    return dynamic_call(eval(wrap_expression(expr, "case", {})));
  }

  object_ref eval(expr::cpp_raw_ref const expr)
  {
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    auto const &interpreter{ __rt_ctx->jit_prc.interpreter };

    /* Parse and execute the C++ code */
    auto parse_res{ interpreter->Parse(expr->code.c_str()) };
    if(!parse_res)
    {
      /* Parse failed - report the error */
      llvm::logAllUnhandledErrors(parse_res.takeError(), llvm::errs(), "cpp/raw parse error: ");
      throw make_box("Failed to parse cpp/raw code").erase();
    }
    if(parse_res)
    {
      auto const translation_unit{ parse_res->TUPart };
      if(translation_unit != nullptr)
      {
        auto &compiler_instance{ *(interpreter->getCompilerInstance()) };
        auto &ast_context{ compiler_instance.getASTContext() };
        auto &source_manager{ compiler_instance.getSourceManager() };
        clang::PrintingPolicy printing_policy{ ast_context.getPrintingPolicy() };
        printing_policy.adjustForCPlusPlus();

        auto same_signature = [](runtime::context::cpp_function_metadata const &lhs,
                                 runtime::context::cpp_function_metadata const &rhs) {
          if(lhs.return_type != rhs.return_type)
          {
            return false;
          }
          if(lhs.arguments.size() != rhs.arguments.size())
          {
            return false;
          }
          for(usize i{}; i < lhs.arguments.size(); ++i)
          {
            if(lhs.arguments[i].type != rhs.arguments[i].type)
            {
              return false;
            }
          }
          return true;
        };

        native_vector<runtime::context::cpp_type_metadata> discovered_types;

        /* Lambda to process a CXXRecordDecl (struct/class/union) */
        auto const process_record = [&](clang::CXXRecordDecl const *record) -> void {
          if(record == nullptr)
          {
            return;
          }
          if(!record->isCompleteDefinition() || record->isImplicit() || record->isLambda()
             || record->isAnonymousStructOrUnion())
          {
            return;
          }

          auto loc(record->getBeginLoc());
          if(!loc.isValid())
          {
            return;
          }
          loc = source_manager.getSpellingLoc(loc);
          auto const file_name(source_manager.getFilename(loc));
          auto const in_main_file(source_manager.isWrittenInMainFile(loc));
          if(!in_main_file && !file_name.empty())
          {
            return;
          }

          auto qualified_name(record->getQualifiedNameAsString());
          if(qualified_name.empty())
          {
            return;
          }
          auto normalized_name(normalize_cpp_entity_name(qualified_name));

          runtime::context::cpp_type_metadata metadata;
          metadata.name = jtl::immutable_string{ normalized_name.data(), normalized_name.size() };
          metadata.qualified_cpp_name
            = jtl::immutable_string{ qualified_name.data(), qualified_name.size() };
          if(record->isUnion())
          {
            metadata.kind = runtime::context::cpp_record_kind::Union;
          }
          else if(record->isClass())
          {
            metadata.kind = runtime::context::cpp_record_kind::Class;
          }
          else
          {
            metadata.kind = runtime::context::cpp_record_kind::Struct;
          }

          for(auto const *field : record->fields())
          {
            if(field == nullptr)
            {
              continue;
            }
            auto field_name(field->getNameAsString());
            if(field_name.empty())
            {
              continue;
            }
            auto const field_type_string(field->getType().getAsString(printing_policy));
            runtime::context::cpp_type_field_metadata field_metadata;
            field_metadata.name = jtl::immutable_string{ field_name.data(), field_name.size() };
            field_metadata.type
              = jtl::immutable_string{ field_type_string.data(), field_type_string.size() };
            metadata.fields.emplace_back(std::move(field_metadata));
          }

          for(auto const *ctor : record->ctors())
          {
            if(ctor == nullptr || ctor->isDeleted() || ctor->isImplicit())
            {
              continue;
            }
            runtime::context::cpp_function_metadata ctor_metadata;
            ctor_metadata.name = jtl::immutable_string{ "constructor" };
            ctor_metadata.return_type = metadata.qualified_cpp_name;

            for(auto const *param : ctor->parameters())
            {
              if(param == nullptr)
              {
                continue;
              }
              auto param_name(param->getNameAsString());
              if(param_name.empty())
              {
                param_name = "arg" + std::to_string(ctor_metadata.arguments.size());
              }
              auto const param_type_string(param->getType().getAsString(printing_policy));
              runtime::context::cpp_function_argument_metadata arg_metadata;
              arg_metadata.name = jtl::immutable_string{ param_name.data(), param_name.size() };
              arg_metadata.type
                = jtl::immutable_string{ param_type_string.data(), param_type_string.size() };
              ctor_metadata.arguments.emplace_back(std::move(arg_metadata));
            }
            metadata.constructors.emplace_back(std::move(ctor_metadata));
          }

          discovered_types.emplace_back(std::move(metadata));
        };

        /* CppInterOp adds a preamble before user code, which offsets Clang line numbers.
         * This constant represents the number of preamble lines to subtract. */
        constexpr unsigned cpp_interop_preamble_lines{ 3 };

        /* Iterate through all contexts to find functions and types */
        native_vector<clang::DeclContext const *> contexts;
        contexts.reserve(8);
        contexts.emplace_back(translation_unit);
        while(!contexts.empty())
        {
          auto const *ctx(contexts.back());
          contexts.pop_back();

          for(auto const *decl : ctx->decls())
          {
            /* Process functions */
            if(auto const *func_decl = llvm::dyn_cast<clang::FunctionDecl>(decl))
            {
              if(!func_decl->hasBody() || !func_decl->hasExternalFormalLinkage())
              {
                continue;
              }

              auto loc(func_decl->getBeginLoc());
              if(!loc.isValid())
              {
                continue;
              }
              loc = source_manager.getSpellingLoc(loc);
              auto const file_name(source_manager.getFilename(loc));
              auto const in_main_file(source_manager.isWrittenInMainFile(loc));
              if(!in_main_file && !file_name.empty())
              {
                continue;
              }

              auto qualified_name(func_decl->getQualifiedNameAsString());
              auto normalized_name(normalize_cpp_entity_name(qualified_name));

              runtime::context::cpp_function_metadata metadata;
              metadata.name
                = jtl::immutable_string{ normalized_name.data(), normalized_name.size() };

              auto const return_type(func_decl->getReturnType());
              auto const return_type_str(return_type.getAsString(printing_policy));
              metadata.return_type
                = jtl::immutable_string{ return_type_str.data(), return_type_str.size() };

              for(auto const *param : func_decl->parameters())
              {
                runtime::context::cpp_function_argument_metadata argument;
                auto const param_name(param->getNameAsString());
                argument.name = jtl::immutable_string{ param_name.data(), param_name.size() };

                auto const param_type(param->getType());
                auto const param_type_str(param_type.getAsString(printing_policy));
                argument.type
                  = jtl::immutable_string{ param_type_str.data(), param_type_str.size() };

                metadata.arguments.emplace_back(std::move(argument));
              }

              /* Store the jank source location where cpp/raw was called,
               * plus the line offset within the C++ code */
              if(expr->source.is_some())
              {
                auto const &src(expr->source.unwrap());
                metadata.origin = src.file;
                /* Calculate the line offset relative to the start of user code.
                 * Subtract the CppInterOp preamble lines and 1 (since line 1 of user code
                 * is the same line as the cpp/raw call). */
                auto const cpp_line(source_manager.getSpellingLineNumber(loc));
                metadata.origin_line
                  = static_cast<std::int64_t>(src.start.line)
                    + static_cast<std::int64_t>(cpp_line - cpp_interop_preamble_lines - 1);
                metadata.origin_column = static_cast<std::int64_t>(src.start.col);
              }

              auto locked_globals{ __rt_ctx->global_cpp_functions.wlock() };
              auto &bucket((*locked_globals)[metadata.name]);
              auto const existing(std::ranges::find_if(bucket, [&](auto const &entry) {
                return same_signature(entry, metadata);
              }));
              if(existing == bucket.end())
              {
                bucket.emplace_back(std::move(metadata));
              }
              else
              {
                *existing = std::move(metadata);
              }
            }

            /* Process global/static variables */
            if(auto const *var_decl = llvm::dyn_cast<clang::VarDecl>(decl))
            {
              /* Only process static variables with file scope */
              if(!var_decl->hasGlobalStorage())
              {
                continue;
              }

              auto loc(var_decl->getBeginLoc());
              if(!loc.isValid())
              {
                continue;
              }
              loc = source_manager.getSpellingLoc(loc);
              auto const in_main_file(source_manager.isWrittenInMainFile(loc));
              auto const file_name(source_manager.getFilename(loc));
              if(!in_main_file && !file_name.empty())
              {
                continue;
              }

              auto qualified_name(var_decl->getQualifiedNameAsString());
              auto normalized_name(normalize_cpp_entity_name(qualified_name));

              runtime::context::cpp_variable_metadata metadata;
              metadata.name
                = jtl::immutable_string{ normalized_name.data(), normalized_name.size() };

              auto const var_type(var_decl->getType());
              auto const var_type_str(var_type.getAsString(printing_policy));
              metadata.type = jtl::immutable_string{ var_type_str.data(), var_type_str.size() };

              /* Store the jank source location where cpp/raw was called,
               * plus the line offset within the C++ code */
              if(expr->source.is_some())
              {
                auto const &src(expr->source.unwrap());
                metadata.origin = src.file;
                /* Calculate the line offset relative to the start of user code.
                 * Subtract the CppInterOp preamble lines and 1 (since line 1 of user code
                 * is the same line as the cpp/raw call). */
                auto const cpp_line(source_manager.getSpellingLineNumber(loc));
                metadata.origin_line
                  = static_cast<std::int64_t>(src.start.line)
                    + static_cast<std::int64_t>(cpp_line - cpp_interop_preamble_lines - 1);
                metadata.origin_column = static_cast<std::int64_t>(src.start.col);
              }

              auto locked_globals{ __rt_ctx->global_cpp_variables.wlock() };
              (*locked_globals)[metadata.name] = std::move(metadata);
            }

            /* Process types (structs/classes/unions) */
            if(auto const *record = llvm::dyn_cast<clang::CXXRecordDecl>(decl))
            {
              process_record(record);
            }

            /* Recurse into nested contexts (namespaces, etc.) */
            if(auto const *inner_ctx = llvm::dyn_cast<clang::DeclContext>(decl))
            {
              contexts.emplace_back(inner_ctx);
            }
          }
        }

        /* Register discovered types */
        if(!discovered_types.empty())
        {
          auto locked_types{ __rt_ctx->global_cpp_types.wlock() };
          for(auto &metadata : discovered_types)
          {
            (*locked_types)[metadata.name] = std::move(metadata);
          }
        }
      }
    }

    /* Execute the parsed code. We must use Execute() on the parse result, not eval_string(),
     * because Parse() already registered the declarations. Calling eval_string() would try
     * to parse the same code again, causing symbol conflicts. */
    if(parse_res)
    {
      auto err(interpreter->Execute(*parse_res));
      if(err)
      {
        llvm::logAllUnhandledErrors(jtl::move(err), llvm::errs(), "cpp/raw execute error: ");
        throw make_box("Failed to execute cpp/raw code").erase();
      }
    }
    else
    {
      /* Parse failed or returned no result - fall back to eval_string */
      __rt_ctx->jit_prc.eval_string(expr->code);
    }
    return runtime::jank_nil;
#else
    (void)expr;
    throw make_box("eval not supported in WASM without CppInterOp").erase();
#endif
  }

  object_ref eval(expr::cpp_type_ref const)
  {
    throw make_box("unsupported eval: cpp_type").erase();
  }

  object_ref eval(expr::cpp_value_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_value", {})));
  }

  object_ref eval(expr::cpp_cast_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_cast", {})));
  }

  object_ref eval(expr::cpp_call_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_call", {})));
  }

  object_ref eval(expr::cpp_constructor_call_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_constructor_call", {})));
  }

  object_ref eval(expr::cpp_member_call_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_member_call", {})));
  }

  object_ref eval(expr::cpp_member_access_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_member_access", {})));
  }

  object_ref eval(expr::cpp_builtin_operator_call_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_builtin_operator_call", {})));
  }

  object_ref eval(expr::cpp_box_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_box", {})));
  }

  object_ref eval(expr::cpp_unbox_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_unbox", {})));
  }

  object_ref eval(expr::cpp_new_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_new", {})));
  }

  object_ref eval(expr::cpp_delete_ref const expr)
  {
    /* TODO: How do we get source info here? Or can we detect this earlier? */
    cpp_util::ensure_convertible(expr).expect_ok();
    return dynamic_call(eval(wrap_expression(expr, "cpp_delete", {})));
  }
}
