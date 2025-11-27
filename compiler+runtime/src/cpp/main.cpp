#include <filesystem>
#include <fstream>

#include <llvm/LineEditor/LineEditor.h>

#include <Interpreter/Compatibility.h>
#include <clang/Interpreter/CppInterOp.h>

#include <jank/read/lex.hpp>
#include <jank/read/parse.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/behavior/callable.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/core/truthy.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/number.hpp>
#include <jank/runtime/detail/type.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/c_api.h>
#include <jank/jit/processor.hpp>
#include <jank/aot/processor.hpp>
#include <jank/profile/time.hpp>
#include <jank/util/scope_exit.hpp>
#include <jank/util/string.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/try.hpp>
#include <jank/environment/check_health.hpp>
#include <jank/runtime/convert/builtin.hpp>

#include <jank/compiler_native.hpp>
#include <jank/perf_native.hpp>
#include <clojure/core_native.hpp>
#include <clojure/string_native.hpp>

#ifdef JANK_PHASE_2
extern "C" jank_object_ref jank_load_clojure_core();
#endif
extern "C" jank_object_ref jank_load_jank_nrepl_server_asio();

namespace jank
{
  using util::cli::opts;

  static void run()
  {
    using namespace jank;
    using namespace jank::runtime;

    {
      profile::timer const timer{ "load clojure.core" };
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    }

    {
      profile::timer const timer{ "eval user code" };
      /* For WASM AOT or LLVM IR codegen, set compile_files_var to true so that eval_string 
       * generates proper module loader functions (jank_load_*).
       * This makes the generated code compatible with the WASM runtime. */
      if(util::cli::opts.codegen == util::cli::codegen_type::wasm_aot
         || util::cli::opts.codegen == util::cli::codegen_type::llvm_ir)
      {
        /* Derive module name from file path (e.g., "wasm-examples/eita.jank" -> "eita") */
        std::filesystem::path const file_path{ util::cli::opts.target_file.c_str() };
        auto const module_name{ file_path.stem().string() };

        context::binding_scope const compile_scope{ obj::persistent_hash_map::create_unique(
          std::make_pair(__rt_ctx->compile_files_var, jank_true),
          std::make_pair(__rt_ctx->current_module_var, make_box(module_name))) };
        util::println("{}", to_code_string(__rt_ctx->eval_file(util::cli::opts.target_file)));

        /* For WASM AOT, generate export wrappers for vars with ^:export metadata */
        if(util::cli::opts.codegen == util::cli::codegen_type::wasm_aot
           && !util::cli::opts.save_cpp_path.empty())
        {
          auto const ns(__rt_ctx->find_ns(make_box<obj::symbol>(module_name)));
          if(!ns.is_nil())
          {
            std::ofstream cpp_out(util::cli::opts.save_cpp_path.c_str(), std::ios::app);
            if(cpp_out.is_open())
            {
              auto const export_kw(__rt_ctx->intern_keyword("export").expect_ok());
              bool has_exports = false;

              /* Scan all vars in the namespace for :export metadata */
              auto const mappings(ns->get_mappings());
              for(auto const &pair : mappings->data)
              {
                /* pair.first is the symbol, pair.second is the var */
                auto const sym(expect_object<obj::symbol>(pair.first));
                auto const var_obj(dyn_cast<var>(pair.second));
                if(var_obj.is_nil() || var_obj->meta.is_none())
                {
                  continue;
                }

                auto const meta(var_obj->meta.unwrap());
                auto const export_val(get(meta, export_kw.erase()));
                if(truthy(export_val))
                {
                  if(!has_exports)
                  {
                    cpp_out << "\n// WASM exports for ^:export vars\n";
                    cpp_out << "// These functions can be called from JavaScript via ccall/cwrap\n\n";
                    has_exports = true;
                  }

                  auto const var_name(sym->name);
                  auto const munged_name(munge(var_name));
                  auto const ns_name(module_name);

                  /* Generate an extern "C" wrapper that takes a double argument
                   * and boxes it as a jank integer before calling the function.
                   * Uses double because JavaScript numbers are IEEE 754 doubles,
                   * and this avoids BigInt conversion issues with long long.
                   * Returns a double for the same reason. */
                  cpp_out << "extern \"C\" double jank_export_" << munged_name << "(double arg) {\n";
                  cpp_out << "  using namespace jank::runtime;\n";
                  cpp_out << "  auto const var = __rt_ctx->find_var(\"" << ns_name << "\", \"" << var_name << "\");\n";
                  cpp_out << "  if(var.is_nil()) { return 0; }\n";
                  cpp_out << "  auto const fn = var->deref();\n";
                  cpp_out << "  auto const boxed_arg = make_box<obj::integer>(static_cast<jank::i64>(arg));\n";
                  cpp_out << "  auto const result = jank::runtime::dynamic_call(fn, boxed_arg);\n";
                  cpp_out << "  // Try to unbox the result as an integer\n";
                  cpp_out << "  auto const int_result = dyn_cast<obj::integer>(result);\n";
                  cpp_out << "  if(!int_result.is_nil()) { return static_cast<double>(int_result->data); }\n";
                  cpp_out << "  // Try to unbox as a real (double)\n";
                  cpp_out << "  auto const real_result = dyn_cast<obj::real>(result);\n";
                  cpp_out << "  if(!real_result.is_nil()) { return real_result->data; }\n";
                  cpp_out << "  // If not a number, return 0\n";
                  cpp_out << "  return 0;\n";
                  cpp_out << "}\n\n";

                  std::cerr << "[jank] Generated WASM export wrapper for: " << var_name << "\n";
                }
              }

              cpp_out.close();
            }
          }
        }
      }
      else
      {
        util::println("{}", to_code_string(__rt_ctx->eval_file(util::cli::opts.target_file)));
      }
    }

    //ankerl::nanobench::Config config;
    //config.mMinEpochIterations = 1000000;
    //config.mOut = &std::cout;
    //config.mWarmup = 10000;


    //ankerl::nanobench::Bench().config(config).run
    //(
    //  "thing",
    //  [&]
    //  {
    //    auto const ret();
    //    ankerl::nanobench::doNotOptimizeAway(ret);
    //  }
    //);
  }

  static void run_main()
  {
    using namespace jank;
    using namespace jank::runtime;

    {
      profile::timer const timer{ "require clojure.core" };
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    }

    {
      profile::timer const timer{ "eval user code" };
      __rt_ctx->load_module("/" + opts.target_module, module::origin::latest).expect_ok();

      auto const main_var(__rt_ctx->find_var(opts.target_module, "-main"));
      if(main_var.is_some())
      {
        /* TODO: Handle the case when `-main` accepts no arg. */
        runtime::detail::native_transient_vector extra_args;
        for(auto const &s : opts.extra_opts)
        {
          extra_args.push_back(make_box<runtime::obj::persistent_string>(s));
        }
        runtime::apply_to(main_var->deref(),
                          make_box<runtime::obj::persistent_vector>(extra_args.persistent()));
      }
      else
      {
        throw std::runtime_error{ util::format("Could not find #'{}/-main function!",
                                               opts.target_module) };
      }
    }
  }

  static void compile_module()
  {
    using namespace jank;
    using namespace jank::runtime;

    if(opts.target_module != "clojure.core")
    {
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    }
    __rt_ctx->compile_module(opts.target_module).expect_ok();
  }

  static void repl()
  {
    using namespace jank;
    using namespace jank::runtime;

    {
      profile::timer const timer{ "require clojure.core" };
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    }

    if(opts.repl_server)
    {
      /* Load and start the nREPL server */
      __rt_ctx->load_module("/jank.nrepl-server.core", module::origin::latest).expect_ok();
      auto const main_var(__rt_ctx->find_var("jank.nrepl-server.core", "-main"));
      if(main_var.is_some())
      {
        dynamic_call(main_var->deref());
      }
      else
      {
        throw std::runtime_error{ "Could not find jank.nrepl-server.core/-main" };
      }
      return;
    }

    dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>("user"));
    dynamic_call(__rt_ctx->intern_var("clojure.core", "refer").expect_ok(),
                 make_box<obj::symbol>("clojure.core"));

    if(!opts.target_module.empty())
    {
      profile::timer const timer{ "load main" };
      __rt_ctx->load_module("/" + opts.target_module, module::origin::latest).expect_ok();
      dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>(opts.target_module));
    }

    auto const get_prompt([](jtl::immutable_string const &suffix) {
      return __rt_ctx->current_ns()->name->to_code_string() + suffix;
    });

    /* By default we are placed in clojure.core ns as of now.
     * TODO: Set default ns to `user` when we are dropped in that ns.*/
    llvm::LineEditor le("jank", ".jank-repl-history");
    le.setPrompt(get_prompt("=> "));
    native_transient_string input{};

    /* We write every REPL expression to a temporary file, which then allows us
     * to later review that for error reporting. We automatically clean it up
     * and we reuse the same file over and over. */
    auto const tmp{ std::filesystem::temp_directory_path() };
    std::string path_tmp{ tmp / "jank-repl-XXXXXX" };
    mkstemp(path_tmp.data());

    /* TODO: Completion. */
    /* TODO: Syntax highlighting. */
    while(auto buf = le.readLine())
    {
      auto &line(*buf);
      util::trim(line);

      if(line.empty())
      {
        util::println("");
        continue;
      }

      if(line.ends_with("\\"))
      {
        input.append(line.substr(0, line.size() - 1));
        input.append("\n");
        le.setPrompt(get_prompt("=>... "));
        continue;
      }

      input += line;

      util::scope_exit const finally{ [&] { std::filesystem::remove(path_tmp); } };
      JANK_TRY
      {
        {
          std::ofstream ofs{ path_tmp };
          ofs << input;
        }

        auto const res(__rt_ctx->eval_file(path_tmp));
        util::println("{}", runtime::to_code_string(res));
      }
      JANK_CATCH(jank::util::print_exception)

      input.clear();
      util::println("");
      le.setPrompt(get_prompt("=> "));
    }
  }

  static void cpp_repl()
  {
    using namespace jank;
    using namespace jank::runtime;

    {
      profile::timer const timer{ "require clojure.core" };
      __rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
    }

    if(!opts.target_module.empty())
    {
      profile::timer const timer{ "load main" };
      __rt_ctx->load_module("/" + opts.target_module, module::origin::latest).expect_ok();
      dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>(opts.target_module));
    }

    llvm::LineEditor le("jank-native", ".jank-native-repl-history");
    le.setPrompt("native> ");
    native_transient_string input{};

    while(auto buf = le.readLine())
    {
      auto &line(*buf);
      util::trim(line);

      if(line.empty())
      {
        continue;
      }

      if(line.ends_with("\\"))
      {
        input.append(line.substr(0, line.size() - 1));
        le.setPrompt("native>... ");
        continue;
      }

      input += line;

      JANK_TRY
      {
        __rt_ctx->jit_prc.eval_string(input);
      }
      JANK_CATCH(jank::util::print_exception)

      input.clear();
      le.setPrompt("native> ");
    }
  }

  static void compile()
  {
    using namespace jank;
    using namespace jank::runtime;

    if(opts.target_module != "clojure.core")
    {
      __rt_ctx->compile_module("clojure.core").expect_ok();
    }
    __rt_ctx->compile_module(opts.target_module).expect_ok();

    jank::aot::processor const aot_prc{};
    aot_prc.compile(opts.target_module).expect_ok();
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): This can only happen if we fail to report an error.
int main(int const argc, char const **argv)
{
  /* TODO: We need an init fn in libjank which sets all of this up so we don't
   * need to duplicate it between here and the tests and so that anyone embedding
   * jank doesn't need to duplicate it in their setup. */
  using namespace jank;
  using namespace jank::runtime;

  return jank_init(argc, argv, /*init_default_ctx=*/false, [](int const argc, char const **argv) {
    auto const parse_result(util::cli::parse(argc, argv));
    if(parse_result.is_err())
    {
      return parse_result.expect_err();
    }

    if(jank::util::cli::opts.gc_incremental)
    {
      GC_enable_incremental();
    }

    profile::configure();
    profile::timer const timer{ "main" };

    if(util::cli::opts.command == util::cli::command::check_health)
    {
      return jank::environment::check_health() ? 0 : 1;
    }

    __rt_ctx = new(GC) runtime::context{};

    jank_load_clojure_core_native();
    jank_load_jank_compiler_native();
    jank_load_jank_perf_native();
    jank_load_jank_nrepl_server_asio();

#ifdef JANK_PHASE_2
    jank_load_clojure_core();
    __rt_ctx->module_loader.set_is_loaded("/clojure.core");
#endif

    Cpp::EnableDebugOutput(false);

    switch(jank::util::cli::opts.command)
    {
      case util::cli::command::run:
        run();
        break;
      case util::cli::command::compile_module:
        compile_module();
        break;
      case util::cli::command::repl:
        repl();
        break;
      case util::cli::command::cpp_repl:
        cpp_repl();
        break;
      case util::cli::command::run_main:
        run_main();
        break;
      case util::cli::command::compile:
        compile();
        break;
      case util::cli::command::check_health:
        break;
    }
    return 0;
  });
}
