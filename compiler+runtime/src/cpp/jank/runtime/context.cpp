#include <exception>
#include <fstream>

#ifndef JANK_TARGET_EMSCRIPTEN
  #include <Interpreter/Compatibility.h>
  #include <clang/Interpreter/CppInterOp.h>
  #include <llvm/ExecutionEngine/Orc/LLJIT.h>
  #include <llvm/Bitcode/BitcodeWriter.h>
  #include <llvm/Target/TargetMachine.h>
  #include <llvm/IR/LegacyPassManager.h>
  #include <llvm/MC/TargetRegistry.h>
  #include <llvm/TargetParser/Host.h>
#endif

#include <jank/read/lex.hpp>
#include <jank/read/parse.hpp>
#include <jank/read/source.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/core/integer_cache.hpp>
#include <jank/runtime/core/real_cache.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/runtime/core.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/core/meta.hpp>
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
  #include <jank/analyze/processor.hpp>
  #include <jank/analyze/expr/primitive_literal.hpp>
  #include <jank/analyze/pass/optimize.hpp>
  #include <jank/evaluate.hpp>
  #include <jank/jit/processor.hpp>
#endif
#ifndef JANK_TARGET_EMSCRIPTEN
  #include <jank/util/clang.hpp>
  #include <jank/util/clang_format.hpp>
  #include <jank/codegen/llvm_processor.hpp>
  #include <jank/codegen/processor.hpp>
#endif
#include <jank/util/environment.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/aot/processor.hpp>
#include <jank/error/codegen.hpp>
#include <jank/error/runtime.hpp>
#include <jank/profile/time.hpp>
#ifdef JANK_IOS_JIT
  #include <jank/compile_server/remote_compile.hpp>
#endif

namespace jank::runtime
{
  /* NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) */
  decltype(context::thread_binding_frames) context::thread_binding_frames{};

  /* NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) */
  context *__rt_ctx{};

  context::context()
    /* We want to initialize __rt_ctx ASAP so other code can start using it. */
    : binary_version{ (__rt_ctx = this, util::binary_version()) }
    , binary_cache_dir{ util::binary_cache_dir(binary_version) }
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    , jit_prc{ binary_version }
#endif
  {
    /* Initialize the integer cache before any make_box calls.
     * This pre-allocates boxed integers in range [-128, 1024] to avoid
     * allocating new objects for commonly used values like loop counters. */
    integer_cache::initialize();
    real_cache::initialize();

    intern_ns(make_box<obj::symbol>("cpp"));
    auto const core(intern_ns(make_box<obj::symbol>("clojure.core")));

    auto const file_sym(make_box<obj::symbol>("*file*"));
    current_file_var = core->intern_var(file_sym);
    current_file_var->bind_root(make_box(read::no_source_path));
    current_file_var->dynamic.store(true);

    auto const ns_sym(make_box<obj::symbol>("*ns*"));
    current_ns_var = core->intern_var(ns_sym);
    current_ns_var->bind_root(core);
    current_ns_var->dynamic.store(true);

    auto const compile_files_sym(make_box<obj::symbol>("*compile-files*"));
    compile_files_var = core->intern_var(compile_files_sym);
    compile_files_var->bind_root(jank_false);
    compile_files_var->dynamic.store(true);

    auto const loaded_libs_sym(make_box<obj::symbol>("*loaded-libs*"));
    loaded_libs_var = core->intern_var(loaded_libs_sym);
    loaded_libs_var->bind_root(make_box<obj::atom>(obj::persistent_sorted_set::empty()));
    loaded_libs_var->dynamic.store(true);

    auto const assert_sym(make_box<obj::symbol>("*assert*"));
    assert_var = core->intern_var(assert_sym);
    assert_var->bind_root(jank_true);
    assert_var->dynamic.store(true);

    /* These are not actually interned. They're extra private. */
    current_module_var
      = make_box<runtime::var>(core, make_box<obj::symbol>("*current-module*"))->set_dynamic(true);
    no_recur_var
      = make_box<runtime::var>(core, make_box<obj::symbol>("*no-recur*"))->set_dynamic(true);
    gensym_env_var
      = make_box<runtime::var>(core, make_box<obj::symbol>("*gensym-env*"))->set_dynamic(true);

    /* TODO: Remove this once native/raw is entirely gone. */
    intern_ns(make_box<obj::symbol>("native"));

    /* This won't be set until clojure.core is loaded. */
    auto const in_ns_sym(make_box<obj::symbol>("clojure.core/in-ns"));
    in_ns_var = intern_var(in_ns_sym).expect_ok();

    push_thread_bindings(obj::persistent_hash_map::create_unique(
                           std::make_pair(current_ns_var, current_ns_var->deref())))
      .expect_ok();
  }

  obj::symbol_ref context::qualify_symbol(obj::symbol_ref const sym) const
  {
    obj::symbol_ref qualified_sym{ sym };
    if(qualified_sym->ns.empty())
    {
      auto const current_ns(expect_object<ns>(current_ns_var->deref()));
      qualified_sym = make_box<obj::symbol>(current_ns->name->name, sym->name);
    }
    return qualified_sym;
  }

  var_ref context::find_var(obj::symbol_ref const sym)
  {
    profile::timer const timer{ "rt find_var" };
    if(!sym->ns.empty())
    {
      ns_ref ns{};
      bool try_alias{ false };
      {
        auto const locked_namespaces(namespaces.rlock());
        auto const found(locked_namespaces->find(make_box<obj::symbol>("", sym->ns)));
        if(found == locked_namespaces->end())
        {
          try_alias = true;
        }
        else
        {
          ns = found->second;
        }
      }

      /* Namespace not found directly, try to resolve as alias in current namespace.
       * This is done outside the lock to avoid potential deadlocks. */
      if(try_alias)
      {
        auto const current_ns_obj(current_ns_var->deref());
        if(current_ns_obj->type != object_type::ns)
        {
          return {};
        }
        auto const current(expect_object<jank::runtime::ns>(current_ns_obj));
        auto const alias(current->find_alias(make_box<obj::symbol>(sym->ns)));
        if(alias.is_nil())
        {
          return {};
        }
        ns = alias;
      }

      return ns->find_var(make_box<obj::symbol>("", sym->name));
    }
    else
    {
      auto const current_ns(expect_object<ns>(current_ns_var->deref()));
      return current_ns->find_var(sym);
    }
  }

  var_ref context::find_var(jtl::immutable_string const &ns, jtl::immutable_string const &name)
  {
    return find_var(make_box<obj::symbol>(ns, name));
  }

  jtl::option<object_ref> context::find_local(obj::symbol_ref const)
  {
    return none;
  }

  jtl::option<object_ref> context::eval_file(jtl::immutable_string const &path)
  {
    profile::timer const timer{ "rt eval_file" };

    profile::enter("phase:read_file");
    auto const file(module::loader::read_file(path));
    profile::exit("phase:read_file");

    if(file.is_err())
    {
      throw file.expect_err();
    }

    binding_scope const preserve{ obj::persistent_hash_map::create_unique(
      std::make_pair(current_file_var, make_box(path))) };

    return eval_string(file.expect_ok().view());
  }

  jtl::option<object_ref> context::eval_string(jtl::immutable_string const &code)
  {
    return eval_string(code, 1, 1);
  }

  jtl::option<object_ref> context::eval_string(jtl::immutable_string const &code,
                                               usize const start_line,
                                               usize const start_col)
  {
    profile::timer const timer{ "rt eval_string" };

#ifdef JANK_IOS_JIT
    /* Check if remote compilation is enabled. If so, send the code to the
     * macOS compile-server for cross-compilation instead of compiling locally.
     * This allows full JIT on iOS without requiring CppInterOp on the device. */
    if(compile_server::is_remote_compile_enabled())
    {
      profile::timer const remote_timer{ "rt eval_string:remote" };

      /* Get the current namespace name for the compilation context */
      auto const current_ns = expect_object<ns>(current_ns_var->deref());
      std::string const ns_name{ current_ns->to_string().data(), current_ns->to_string().size() };
      std::string const code_str{ code.data(), code.size() };

      /* Send code to compile-server and receive object file */
      auto const response = compile_server::remote_compile(code_str, ns_name);

      if(!response.success)
      {
        throw error::internal_codegen_failure(
          jtl::immutable_string{ "Remote compilation failed: " + response.error });
      }

      if(response.object_data.empty())
      {
        /* No object generated - might be a no-op form */
        return jank_nil();
      }

      /* Load the object file into the JIT */
      {
        profile::timer const load_timer{ "rt eval_string:remote:load" };
        auto load_result
          = jit_prc.load_object(reinterpret_cast<char const *>(response.object_data.data()),
                                response.object_data.size(),
                                response.entry_symbol);

        if(!load_result)
        {
          throw error::internal_codegen_failure(
            jtl::immutable_string{ "Failed to load remote-compiled object file" });
        }
      }

      /* Find and call the entry symbol */
      {
        profile::timer const call_timer{ "rt eval_string:remote:call" };
        jtl::immutable_string const entry_sym{ response.entry_symbol };
        auto const fn_result = jit_prc.find_symbol(entry_sym);
        if(fn_result.is_err())
        {
          throw error::internal_codegen_failure(jtl::immutable_string{
            "Failed to find entry symbol in remote-compiled object: " + response.entry_symbol });
        }

        auto const fn_ptr = reinterpret_cast<object *(*)()>(fn_result.expect_ok());
        return fn_ptr();
      }
    }
#endif /* JANK_IOS_JIT */

    read::lex::processor l_prc{ code, start_line, start_col };
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

    bool no_op{ true };
    object_ref ret{ jank_nil() };
    native_vector<object_ref> forms{};
    for(auto const &form : p_prc)
    {
      if(no_op && form.expect_ok().is_none())
      {
        continue;
      }

      no_op = false;

#ifdef JANK_TARGET_WASM
      /* WASM doesn't support JIT - just return the parsed form */
      ret = form.expect_ok().unwrap().ptr;
      forms.emplace_back(ret);
#else
      auto const parsed_form = form.expect_ok().unwrap().ptr;

      profile::enter("phase:analyze");
      analyze::processor an_prc;
      auto expr = an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok();
      profile::exit("phase:analyze");

      profile::enter("phase:optimize");
      expr = analyze::pass::optimize(expr);
      profile::exit("phase:optimize");

      profile::enter("phase:eval");
      ret = evaluate::eval(expr);
      profile::exit("phase:eval");

      forms.emplace_back(parsed_form);
#endif
    }

    if(no_op)
    {
      return jtl::none;
    }

#ifdef JANK_TARGET_WASM
    /* WASM doesn't support module compilation */
    (void)forms;
#elif defined(JANK_TARGET_EMSCRIPTEN)
    if(truthy(compile_files_var->deref()))
    {
      throw error::internal_codegen_failure(
        "Module compilation is unavailable when targeting emscripten.");
    }
#else
    /* When compiling, we analyze twice. This is because eval will modify its expression
     * in order to wrap it in a function. Undoing this is arduous and error prone, so
     * we just don't bother.
     *
     * Furthermore, module compilation may be different from JIT compilation, since it's
     * targeted at AOT and doesn't have access to what's loaded in the JIT runtime.
     *
     * Skip codegen entirely when --list-modules is used - we only need to discover dependencies. */
    if(truthy(compile_files_var->deref()) && !util::cli::opts.list_modules)
    {
      profile::timer const timer{ "rt compile-module" };
      auto const &module(runtime::to_string(current_module_var->deref()));
      auto const name{ module::module_to_load_function(module) };

      if(forms.empty())
      {
        forms.emplace_back(jank_nil());
      }

      auto const form{ runtime::conj(
        runtime::conj(runtime::conj(make_box<obj::native_vector_sequence>(jtl::move(forms)),
                                    obj::persistent_vector::empty()),
                      make_box<obj::symbol>(name)),
        make_box<obj::symbol>("fn*")) };
      auto const expr(analyze::pass::optimize(
        an_prc.analyze(form, analyze::expression_position::statement).expect_ok()));
      auto const fn{ static_box_cast<analyze::expr::function>(expr) };
      fn->unique_name = name;

      if(util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
      {
        codegen::llvm_processor const cg_prc{ fn, module, codegen::compilation_target::module };
        cg_prc.gen().expect_ok();
        cg_prc.optimize();

        /* Save LLVM IR to a file if requested */
        if(util::cli::opts.save_llvm_ir || !util::cli::opts.save_llvm_ir_path.empty())
        {
          jtl::immutable_string ll_path;
          if(!util::cli::opts.save_llvm_ir_path.empty())
          {
            ll_path = util::cli::opts.save_llvm_ir_path;
          }
          else
          {
            ll_path = util::format("{}/{}.ll", binary_cache_dir, module::module_to_path(module));
          }
          auto const parent_path = std::filesystem::path{ ll_path.c_str() }.parent_path();
          if(!parent_path.empty())
          {
            std::filesystem::create_directories(parent_path);
          }

          std::error_code ec;
          llvm::raw_fd_ostream ll_out(ll_path.c_str(), ec, llvm::sys::fs::OF_Text);
          if(!ec)
          {
            cg_prc.get_module().getModuleUnlocked()->print(ll_out, nullptr);
            std::cerr << "[jank] Saved LLVM IR to: " << ll_path << "\n";
          }
          else
          {
            std::cerr << "[jank] Failed to save LLVM IR to: " << ll_path << ": " << ec.message()
                      << "\n";
          }
        }

        write_module(cg_prc.get_module_name(), "", cg_prc.get_module().getModuleUnlocked())
          .expect_ok();
      }
      else
      {
        profile::timer const timer{ "rt compile-module parse + write" };
        /* For WASM AOT, use the wasm_aot target; otherwise use module target */
        auto const cg_target = (util::cli::opts.codegen == util::cli::codegen_type::wasm_aot)
          ? codegen::compilation_target::wasm_aot
          : codegen::compilation_target::module;
        codegen::processor cg_prc{ fn, module, cg_target };
        auto const code{ cg_prc.declaration_str() };
        auto module_name{ runtime::to_string(current_module_var->deref()) };

        /* Save generated C++ to a file for inspection/WASM compilation. */
        if(util::cli::opts.save_cpp || !util::cli::opts.save_cpp_path.empty()
           || util::cli::opts.codegen == util::cli::codegen_type::wasm_aot)
        {
          jtl::immutable_string cpp_path;
          if(!util::cli::opts.save_cpp_path.empty())
          {
            cpp_path = util::cli::opts.save_cpp_path;
          }
          else
          {
            cpp_path = util::format("{}/{}.cpp", binary_cache_dir, module::module_to_path(module));
          }
          auto const parent_path = std::filesystem::path{ cpp_path.c_str() }.parent_path();
          if(!parent_path.empty())
          {
            std::filesystem::create_directories(parent_path);
          }

          /* For WASM AOT, we need to add includes at the top of the file. */
          bool const is_wasm_aot = (util::cli::opts.codegen == util::cli::codegen_type::wasm_aot);

          /* Use truncate mode to overwrite the file each time, not append */
          std::ofstream cpp_out(cpp_path.c_str(), std::ios::trunc);
          if(cpp_out.is_open())
          {
            /* Write WASM AOT includes at the start of file */
            if(is_wasm_aot)
            {
              cpp_out << "// WASM AOT generated code - requires jank runtime headers\n";
              cpp_out << "#include <jank/runtime/context.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/jit_function.hpp>\n";
              cpp_out << "#include <jank/runtime/core.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_hash_set.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_array_map.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_hash_map.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_sorted_map.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_sorted_set.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/persistent_vector.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/range.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/integer_range.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/ratio.hpp>\n";
              /* Include keyword and symbol for static_cast conversions in vectors */
              cpp_out << "#include <jank/runtime/obj/keyword.hpp>\n";
              cpp_out << "#include <jank/runtime/obj/symbol.hpp>\n";
              /* Include convert for type conversions (bool, int, etc.) */
              cpp_out << "#include <jank/runtime/convert/builtin.hpp>\n";
              cpp_out << "#include <boost/multiprecision/cpp_int.hpp>\n";
              /* Include scope_exit for finally blocks */
              cpp_out << "#include <jank/util/scope_exit.hpp>\n";
              /* Include meta for reset_meta used by cpp/box */
              cpp_out << "#include <jank/runtime/core/meta.hpp>\n";
              /* Include opaque_box for C++ FFI interop boxing */
              cpp_out << "#include <jank/runtime/obj/opaque_box.hpp>\n";
              /* Include C API for jank_unbox_lazy_source used by cpp/unbox */
              cpp_out << "#include <jank/c_api.h>\n";

              /* Include native headers from (:require ["header.h" :as alias]) */
              auto const curr_ns{ current_ns() };
              auto const native_aliases{ curr_ns->native_aliases_snapshot() };
              if(!native_aliases.empty())
              {
                cpp_out << "\n/* Native headers from :require directives */\n";
                native_set<jtl::immutable_string> seen_includes;
                for(auto const &alias : native_aliases)
                {
                  /* Deduplicate includes - same header may be required with different aliases */
                  if(seen_includes.insert(alias.include_directive).second)
                  {
                    cpp_out << "#include " << alias.include_directive.c_str() << "\n";
                  }
                }
              }
              cpp_out << "\n";
            }

            cpp_out << code << "\n\n";
            cpp_out.close();
            std::cerr << "[jank] Saved generated C++ to: " << cpp_path << "\n";
          }
        }

        /* For WASM AOT, we only generate C++ code - skip JIT compilation.
         * The generated C++ will be compiled by emscripten separately. */
        if(util::cli::opts.codegen == util::cli::codegen_type::wasm_aot)
        {
          /* Don't JIT compile for WASM AOT - just save the C++ file */
          std::cerr << "[jank] WASM AOT mode: skipping JIT compilation\n";
        }
        else
        {
          auto parse_res{ jit_prc.interpreter->Parse({ code.data(), code.size() }) };
          if(!parse_res)
          {
            jtl::immutable_string const res{ "Unable to compile generated C++ source." };
            llvm::logAllUnhandledErrors(parse_res.takeError(), llvm::errs(), "error: ");
            llvm::errs().flush();
            throw error::internal_codegen_failure(res);
          }
          auto &partial_tu{ parse_res.get() };
          write_module(module_name, code, partial_tu.TheModule.get()).expect_ok();
        }
      }
    }
#endif

    return ret;
  }

#ifdef JANK_TARGET_EMSCRIPTEN
  jtl::result<void, error_ref> context::eval_cpp_string(jtl::immutable_string const &) const
  {
    return error::runtime_invalid_cpp_eval();
  }
#else
  jtl::result<void, error_ref> context::eval_cpp_string(jtl::immutable_string const &code) const
  {
    profile::timer const timer{ "rt eval_cpp_string" };

    /* Capture stderr output (C++ compilation errors) and forward them through
     * the output redirection system so they appear in the IDE REPL. */
    std::string parse_error_msg;
    std::string exec_error_msg;
    bool parse_failed = false;
    bool exec_failed = false;

    {
      scoped_stderr_redirect const stderr_redirect{};

      auto parse_res{ jit_prc.interpreter->Parse({ code.data(), code.size() }) };
      if(!parse_res)
      {
        /* Capture error to string */
        llvm::raw_string_ostream error_stream(parse_error_msg);
        llvm::logAllUnhandledErrors(parse_res.takeError(), error_stream, "error: ");
        error_stream.flush();
        parse_failed = true;
      }
      else
      {
        auto &partial_tu{ parse_res.get() };

        /* Writing the module before executing it because `llvm::Interpreter::Execute`
         * moves the `llvm::Module` held in the `PartialTranslationUnit`. */
        if(truthy(compile_files_var->deref()))
        {
          auto module_name{ runtime::to_string(current_module_var->deref()) };
          write_module(module_name, code, partial_tu.TheModule.get()).expect_ok();
        }

        auto exec_res(jit_prc.interpreter->Execute(partial_tu));
        if(exec_res)
        {
          llvm::raw_string_ostream error_stream(exec_error_msg);
          llvm::logAllUnhandledErrors(std::move(exec_res), error_stream, "error: ");
          error_stream.flush();
          exec_failed = true;
        }
      }
    } // stderr_redirect scope ends here

    /* Check for errors after the redirect scope ends.
     * Error messages are captured to strings and can be accessed if needed. */
    if(parse_failed)
    {
      return error::runtime_invalid_cpp_eval();
    }
    if(exec_failed)
    {
      return error::runtime_invalid_cpp_eval();
    }

    return ok();
  }
#endif

  object_ref context::read_string(jtl::immutable_string const &code)
  {
    profile::timer const timer{ "rt read_string" };

    /* When reading an arbitrary string, we don't want the last *current-file* to
     * be set as source file, so we need to bind it to nil. */
    binding_scope const preserve{ obj::persistent_hash_map::create_unique(
      std::make_pair(current_file_var, jank_nil())) };

    read::lex::processor l_prc{ code };
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

    object_ref ret{ jank_nil() };
    for(auto const &form : p_prc)
    {
      ret = form.expect_ok().unwrap().ptr;
    }

    return ret;
  }

#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
  native_vector<analyze::expression_ref>
  context::analyze_string(jtl::immutable_string const &code, bool const eval)
  {
    profile::timer const timer{ "rt analyze_string" };
    read::lex::processor l_prc{ code };
    read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

    native_vector<analyze::expression_ref> ret{};
    for(auto const &form : p_prc)
    {
      if(eval)
      {
        auto const expr(
          an_prc.analyze(form.expect_ok().unwrap().ptr, analyze::expression_position::statement));
        if(expr.is_err())
        {
          util::println("{}", expr.expect_err()->message);
        }
        evaluate::eval(analyze::pass::optimize(expr.expect_ok()));
      }

      auto const expr(analyze::pass::optimize(
        an_prc.analyze(form.expect_ok().unwrap().ptr, analyze::expression_position::statement)
          .expect_ok()));
      ret.emplace_back(expr);
    }

    return ret;
  }
#endif

  jtl::result<void, error_ref>
  context::load_module(jtl::immutable_string const &module, module::origin const ori)
  {
    auto const ns(current_ns());

    jtl::immutable_string absolute_module;
    if(module.starts_with('/'))
    {
      absolute_module = module.substr(1);
    }
    else
    {
      absolute_module = module::nest_module(ns->to_string(), module);
    }

    /* When we load a module, the `*ns*` var is still set to the previous module.
     * In the `clojure.core/ns` macro, `in-ns` is called that sets the value of the
     * current ns to the module being loaded. To avoid overwriting the previous `ns` value, `current_ns_var`
     * binding is pushed in the context, and then `in-ns` sets the value of `*ns*` var in
     * the new binding scope. */
    binding_scope const preserve{ obj::persistent_hash_map::create_unique(
      std::make_pair(current_ns_var, ns),
      std::make_pair(current_module_var, make_box(absolute_module))) };

    try
    {
      return module_loader.load(absolute_module, ori);
    }
    catch(std::exception const &e)
    {
      return error::runtime_unable_to_load_module(e.what());
    }
    catch(object_ref const e)
    {
      return error::runtime_unable_to_load_module(runtime::to_code_string(e));
    }
    catch(error_ref const e)
    {
      return e;
    }
  }

  jtl::result<void, error_ref> context::compile_module(jtl::immutable_string const &module)
  {
    module_dependencies.clear();

    binding_scope const preserve{ obj::persistent_hash_map::create_unique(
      std::make_pair(compile_files_var, jank_true)) };

    /* For WASM AOT compilation, we need to force loading from source to recompile
     * the module rather than using cached object files. */
    auto const ori = (util::cli::opts.codegen == util::cli::codegen_type::wasm_aot)
      ? module::origin::source
      : module::origin::latest;

    return load_module(util::format("/{}", module), ori);
  }

#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
  object_ref context::eval(object_ref const o)
  {
  #ifdef JANK_IOS_JIT
    /* On iOS JIT mode with remote compilation enabled, delegate to eval_string
     * which handles the compile server communication. This is necessary because
     * local analysis can't resolve C++ interop symbols (headers aren't loaded
     * on iOS). By going through eval_string, the code is sent to the compile
     * server which has full C++ header access via CppInterOp. */
    if(compile_server::is_remote_compile_enabled())
    {
      /* Convert the form to a string representation that can be sent to
       * the compile server. We use to_code_string for proper formatting. */
      auto const code = runtime::to_code_string(o);
      return eval_string(code);
    }
  #endif

    auto const expr(
      analyze::pass::optimize(an_prc.analyze(o, analyze::expression_position::value).expect_ok()));
    return evaluate::eval(expr);
  }
#else
  object_ref context::eval(object_ref const o)
  {
    /* WASM doesn't support JIT evaluation - just return the object */
    return o;
  }
#endif

#ifdef JANK_TARGET_EMSCRIPTEN
  jtl::string_result<void> context::write_module(jtl::immutable_string const &module_name,
                                                 jtl::immutable_string const &,
                                                 jtl::ref<llvm::Module> const &) const
  {
    return err(
      util::format("Writing modules is unsupported on emscripten (module '{}').", module_name));
  }
#else
  jtl::immutable_string
  context::get_output_module_name(jtl::immutable_string const &module_name) const
  {
    char const *ext{};
    switch(util::cli::opts.output_target)
    {
      case util::cli::compilation_target::llvm_ir:
        ext = "ll";
        break;
      case util::cli::compilation_target::cpp:
        ext = "cpp";
        break;
      case util::cli::compilation_target::object:
        ext = "o";
        break;
      case util::cli::compilation_target::unspecified:
      default:
        throw error::internal_runtime_failure(
          util::format("Unable to determine output module name, given output target '{}'.",
                       util::cli::compilation_target_str(util::cli::opts.output_target)));
    }

    return util::cli::opts.output_module_filename.empty()
      ? util::format("{}/{}.{}", binary_cache_dir, module::module_to_path(module_name), ext)
      : jtl::immutable_string{ util::cli::opts.output_module_filename };
  }

  jtl::string_result<void> context::write_module(jtl::immutable_string const &module_name,
                                                 jtl::immutable_string const &cpp_code,
                                                 jtl::ref<llvm::Module> const &module) const
  {
    profile::timer const timer{ util::format("write_module {}", module_name) };
    std::filesystem::path const module_path{ get_output_module_name(module_name) };
    auto const &module_dir{ module_path.parent_path() };
    if(!module_dir.empty())
    {
      std::filesystem::create_directories(module_dir);
    }

    switch(util::cli::opts.output_target)
    {
      case util::cli::compilation_target::cpp:
        {
          std::ofstream ofs{ module_path.c_str() };
          ofs << "#include <jank/prelude.hpp>\n";
          ofs << cpp_code;
          return ok();
        }
      case util::cli::compilation_target::llvm_ir:
        {
          std::error_code file_error{};
          llvm::raw_fd_ostream os(module_path.c_str(),
                                  file_error,
                                  llvm::sys::fs::OpenFlags::OF_None);
          if(file_error)
          {
            return err(util::format("Failed to open module file '{}' with error '{}'.",
                                    module_path.c_str(),
                                    file_error.message()));
          }
          module->print(os, nullptr);
          return ok();
        }
      case util::cli::compilation_target::object:
        {
          /* TODO: Is there a better place for this block of code? */
          std::error_code file_error{};
          llvm::raw_fd_ostream os(module_path.c_str(),
                                  file_error,
                                  llvm::sys::fs::OpenFlags::OF_None);
          if(file_error)
          {
            return err(util::format("Failed to open module file '{}' with error '{}'.",
                                    module_path.c_str(),
                                    file_error.message()));
          }
          //module->print(llvm::outs(), nullptr);

          auto const target_triple{ util::default_target_triple() };
          std::string target_error;
          auto const target{ llvm::TargetRegistry::lookupTarget(target_triple.c_str(),
                                                                target_error) };
          if(!target)
          {
            return err(target_error);
          }
          llvm::TargetOptions const opt;
          auto const target_machine{ target->createTargetMachine(
            llvm::Triple{ target_triple.c_str() },
            "generic",
            "",
            opt,
            llvm::Reloc::PIC_,
            llvm::CodeModel::Large,
            llvm::CodeGenOptLevel::Default) };
          if(!target_machine)
          {
            return err(util::format("Failed to create target machine for '{}'.", target_triple));
          }
          llvm::legacy::PassManager pass;

          if(target_machine->addPassesToEmitFile(pass,
                                                 os,
                                                 nullptr,
                                                 llvm::CodeGenFileType::ObjectFile))
          {
            return err(
              util::format("Failed to write module to object file for '{}'.", target_triple));
          }

          pass.run(*module);
          return ok();
        }
      case util::cli::compilation_target::unspecified:
      default:
        return err(util::format("Unable to write module, given output target '{}'.",
                                util::cli::compilation_target_str(util::cli::opts.output_target)));
    }
  }
#endif

  jtl::immutable_string context::unique_namespaced_string() const
  {
    return unique_namespaced_string("G_");
  }

  jtl::immutable_string context::unique_namespaced_string(jtl::immutable_string const &prefix) const
  {
    static jtl::immutable_string const dot{ "\\." };
    auto const ns{ current_ns() };
    return util::format("{}-{}-{}",
                        runtime::munge_and_replace(ns->name->get_name(), dot, "_"),
                        prefix.c_str(),
                        ++ns->symbol_counter);
  }

  jtl::immutable_string context::unique_string() const
  {
    return unique_string("G_");
  }

  jtl::immutable_string context::unique_string(jtl::immutable_string const &prefix) const
  {
    auto const ns{ current_ns() };
    return util::format("{}-{}", prefix.c_str(), ++ns->symbol_counter);
  }

  obj::symbol_ref context::unique_symbol() const
  {
    return unique_symbol("G-");
  }

  obj::symbol_ref context::unique_symbol(jtl::immutable_string const &prefix) const
  {
    return make_box<obj::symbol>("", unique_namespaced_string(prefix));
  }

  ns_ref context::intern_ns(jtl::immutable_string const &name)
  {
    return intern_ns(make_box<obj::symbol>(name));
  }

  ns_ref context::intern_ns(obj::symbol_ref const sym)
  {
    if(!sym->ns.empty())
    {
      throw std::runtime_error{ util::format("Can't intern ns. Sym is qualified: {}",
                                             sym->to_string()) };
    }
    auto locked_namespaces(namespaces.wlock());
    auto const found(locked_namespaces->find(sym));
    if(found != locked_namespaces->end())
    {
      return found->second;
    }

    auto const result(locked_namespaces->emplace(sym, make_box<ns>(sym)));
    return result.first->second;
  }

  ns_ref context::remove_ns(obj::symbol_ref const sym)
  {
    auto locked_namespaces(namespaces.wlock());
    auto const found(locked_namespaces->find(sym));
    if(found != locked_namespaces->end())
    {
      auto const ret(found->second);
      locked_namespaces->erase(found);
      return ret;
    }
    return {};
  }

  ns_ref context::find_ns(obj::symbol_ref const sym)
  {
    auto locked_namespaces(namespaces.rlock());
    auto const found(locked_namespaces->find(sym));
    if(found != locked_namespaces->end())
    {
      return found->second;
    }
    return {};
  }

  ns_ref context::resolve_ns(obj::symbol_ref const target)
  {
    auto const ns(current_ns());
    auto alias(ns->find_alias(target));
    if(alias.is_some())
    {
      return alias;
    }

    return find_ns(target);
  }

  ns_ref context::current_ns() const
  {
    return expect_object<ns>(current_ns_var->deref());
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_var(jtl::immutable_string const &qualified_name)
  {
    return intern_var(make_box<obj::symbol>(qualified_name));
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_var(jtl::immutable_string const &ns, jtl::immutable_string const &name)
  {
    return intern_var(make_box<obj::symbol>(ns, name));
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_var(obj::symbol_ref const qualified_name)
  {
    profile::timer const timer{ "intern_var" };
    if(qualified_name->ns.empty())
    {
      return err(
        util::format("Can't intern var. Sym isn't qualified: {}", qualified_name->to_string()));
    }

    auto locked_namespaces(namespaces.wlock());
    obj::symbol_ref const ns_sym{ make_box<obj::symbol>(qualified_name->ns) };
    auto found_ns(locked_namespaces->find(ns_sym));
    if(found_ns == locked_namespaces->end())
    {
      /* Auto-create namespace if it doesn't exist - needed for AOT/WASM mode
       * where vars may be interned in constructors before in-ns is called */
      auto const result(locked_namespaces->emplace(ns_sym, make_box<ns>(ns_sym)));
      found_ns = result.first;
    }

    return ok(found_ns->second->intern_var(qualified_name));
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_owned_var(jtl::immutable_string const &ns, jtl::immutable_string const &name)
  {
    return intern_owned_var(make_box<obj::symbol>(ns, name));
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_owned_var(jtl::immutable_string const &qualified_name)
  {
    return intern_owned_var(make_box<obj::symbol>(qualified_name));
  }

  jtl::result<var_ref, jtl::immutable_string>
  context::intern_owned_var(obj::symbol_ref const qualified_sym)
  {
    /* TODO: Clean up duplication between this and intern_var. */
    profile::timer const timer{ "intern_var" };
    if(qualified_sym->ns.empty())
    {
      return err(
        util::format("Can't intern var. Sym isn't qualified: {}", qualified_sym->to_string()));
    }

    auto locked_namespaces(namespaces.wlock());
    obj::symbol_ref const ns_sym{ make_box<obj::symbol>(qualified_sym->ns) };
    auto const found_ns(locked_namespaces->find(ns_sym));
    if(found_ns == locked_namespaces->end())
    {
      return err(util::format("Can't intern var. Namespace doesn't exist: {}", qualified_sym->ns));
    }

    return ok(found_ns->second->intern_owned_var(qualified_sym));
  }

  jtl::result<obj::keyword_ref, jtl::immutable_string>
  context::intern_keyword(jtl::immutable_string const &ns,
                          jtl::immutable_string const &name,
                          bool const resolved)
  {
    jtl::immutable_string resolved_ns{ ns };
    if(!resolved)
    {
      /* The ns will be an ns alias. */
      if(!ns.empty())
      {
        auto const resolved(current_ns()->find_alias(make_box<obj::symbol>(ns)));
        if(resolved.is_nil())
        {
          return err(util::format("Unable to resolve namespace alias '{}'", ns));
        }
        resolved_ns = resolved->name->name;
      }
      else
      {
        auto const current_ns(expect_object<jank::runtime::ns>(current_ns_var->deref()));
        resolved_ns = current_ns->name->name;
      }
    }
    return intern_keyword(resolved_ns.empty() ? name : util::format("{}/{}", resolved_ns, name));
  }

  jtl::result<obj::keyword_ref, jtl::immutable_string>
  context::intern_keyword(jtl::immutable_string const &s)
  {
    profile::timer const timer{ "rt intern_keyword" };

    auto locked_keywords(keywords.wlock());
    auto const found(locked_keywords->find(s));
    if(found != locked_keywords->end())
    {
      return found->second;
    }

    auto const res(
      locked_keywords->emplace(s, make_box<obj::keyword>(detail::must_be_interned{}, s)));
    return res.first->second;
  }

  object_ref context::macroexpand1(object_ref const o)
  {
    profile::timer const timer{ "rt macroexpand1" };
    return visit_seqable(
      [this](auto const typed_o) -> object_ref {
        using T = typename jtl::decay_t<decltype(typed_o)>::value_type;

        if constexpr(!behavior::sequenceable<T>)
        {
          return typed_o;
        }
        else
        {
          auto const first_sym_obj(dyn_cast<obj::symbol>(first(typed_o)));
          if(first_sym_obj.is_nil())
          {
            return typed_o;
          }

          auto const var(find_var(first_sym_obj));
          /* None means it's not a var, so not a macro. No meta means no :macro set. */
          if(var.is_nil() || var->meta.is_none())
          {
            return typed_o;
          }

          auto const meta(var->meta.unwrap());
          auto const found_macro(get(meta, intern_keyword("", "macro", true).expect_ok()));
          if(found_macro.is_nil() || !truthy(found_macro))
          {
            return typed_o;
          }

          /* TODO: Provide &env. */
          auto const args(cons(cons(rest(typed_o), jank_nil()), typed_o));
          return apply_to(var->deref(), args);
        }
      },
      [=]() { return o; },
      o);
  }

  object_ref context::macroexpand(object_ref const o)
  {
    auto expanded(macroexpand1(o));
    if(expanded != o)
    {
      /* If we've actually expanded `o` into something else, it's helpful to update the meta
       * on the expanded data to tie it back to the original form. */
      auto const source{ object_source(o) };
      if(source != read::source::unknown())
      {
        auto meta{ runtime::meta(expanded) };
        auto const macro_kw{ __rt_ctx->intern_keyword("jank/macro-expansion").expect_ok() };
        meta = runtime::assoc(meta, macro_kw, o);
        expanded = with_meta_graceful(expanded, meta);
      }

      return macroexpand(expanded);
    }

    return o;
  }

  context::binding_scope::binding_scope()
  {
    __rt_ctx->push_thread_bindings().expect_ok();
  }

  context::binding_scope::binding_scope(obj::persistent_hash_map_ref const bindings)
  {
    __rt_ctx->push_thread_bindings(bindings).expect_ok();
  }

  context::binding_scope::~binding_scope()
  {
    try
    {
      __rt_ctx->pop_thread_bindings().expect_ok();
    }
    catch(...)
    {
      util::println("Exception caught while destructing binding_scope");
    }
  }

  jtl::string_result<void> context::push_thread_bindings()
  {
    auto bindings(obj::persistent_hash_map::empty());
    auto &tbfs(thread_binding_frames[std::this_thread::get_id()]);
    if(!tbfs.empty())
    {
      bindings = tbfs.front().bindings;
    }
    /* Nothing to preserve, if there are no current bindings. */
    else
    {
      return ok();
    }

    return push_thread_bindings(bindings);
  }

  jtl::string_result<void> context::push_thread_bindings(object_ref const bindings)
  {
    if(bindings->type != object_type::persistent_hash_map)
    {
      return err(util::format("invalid thread binding map (must be hash map): {}",
                              runtime::to_code_string(bindings)));
    }

    return push_thread_bindings(expect_object<obj::persistent_hash_map>(bindings));
  }

  jtl::string_result<void>
  context::push_thread_bindings(obj::persistent_hash_map_ref const bindings)
  {
    thread_binding_frame frame{ obj::persistent_hash_map::empty() };
    auto const thread_id{ std::this_thread::get_id() };
    auto &tbfs(thread_binding_frames[thread_id]);
    if(!tbfs.empty())
    {
      frame.bindings = tbfs.front().bindings;
    }

    for(auto it(bindings->fresh_seq()); it.is_some(); it = it->next_in_place())
    {
      auto const entry(it->first());
      auto const var(expect_object<var>(entry->data[0]));
      if(!var->dynamic.load())
      {
        return err(
          util::format("Can't dynamically bind non-dynamic var: {}", var->to_code_string()));
      }

      /* XXX: Once this is set to true, here, it's never unset. */
      var->thread_bound.store(true);

      /* The binding may already be a thread binding if we're just pushing the previous
       * bindings again to give a scratch pad for some upcoming code. */
      if(entry->data[1]->type == object_type::var_thread_binding)
      {
        frame.bindings = frame.bindings->assoc(
          var,
          make_box<var_thread_binding>(expect_object<var_thread_binding>(entry->data[1])->value,
                                       thread_id));
      }
      else
      {
        frame.bindings
          = frame.bindings->assoc(var, make_box<var_thread_binding>(entry->data[1], thread_id));
      }
    }

    tbfs.push_front(std::move(frame));
    return ok();
  }

  jtl::string_result<void> context::pop_thread_bindings()
  {
    auto &tbfs(thread_binding_frames[std::this_thread::get_id()]);
    if(tbfs.empty())
    {
      return err("Mismatched thread binding pop");
    }

    tbfs.pop_front();

    return ok();
  }

  obj::persistent_hash_map_ref context::get_thread_bindings() const
  {
    auto const &tbfs(thread_binding_frames[std::this_thread::get_id()]);
    if(tbfs.empty())
    {
      return obj::persistent_hash_map::empty();
    }
    return tbfs.front().bindings;
  }

  jtl::option<thread_binding_frame> context::current_thread_binding_frame()
  {
    auto &tbfs(thread_binding_frames[std::this_thread::get_id()]);
    if(tbfs.empty())
    {
      return none;
    }
    return tbfs.front();
  }
}
