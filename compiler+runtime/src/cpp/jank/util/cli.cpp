#include <CLI/CLI.hpp>

#include <jank/util/cli.hpp>
#include <jank/util/fmt.hpp>
#include <jank/runtime/module/loader.hpp>

namespace jank::util::cli
{
  /* NOLINTNEXTLINE */
  options opts;

  static std::string make_default(std::string const &input)
  {
    return "default: " + input;
  }

  jtl::result<void, int> parse(int const argc, char const **argv)
  {
    CLI::App cli{ "jank compiler" };

    cli.set_help_flag("-h,--help", "Print this help message and exit.");

    /* Runtime. */
    cli.add_option(
      "--module-path",
      opts.module_path,
      util::format(
        "A {} separated list of directories, JAR files, and ZIP files to search for modules.",
        runtime::module::loader::module_separator_name));
    cli.add_flag("--profile", opts.profiler_enabled, "Enable compiler and runtime profiling.");
    cli.add_flag("--profile-fns",
                 opts.profiler_fns_enabled,
                 "Automatically profile all function calls (implies --profile).");
    cli.add_flag("--profile-core",
                 opts.profiler_core_enabled,
                 "Also profile clojure.core functions (implies --profile-fns).");
    cli.add_flag("--profile-interop",
                 opts.profiler_interop_enabled,
                 "Profile cpp/ interop operations like box/unbox (implies --profile).");
    cli
      .add_option("--profile-sample",
                  opts.profiler_sample_rate,
                  "Sample 1 in N events (e.g., 10=10%, 100=1%). 0 or omit for all events.")
      ->default_str("0");
    cli
      .add_option("--profile-output",
                  opts.profiler_file,
                  "The file to write profile entries (will be overwritten).")
      ->default_str(make_default(opts.profiler_file));
    cli.add_flag("--perf", opts.perf_profiling_enabled, "Enable Linux perf event sampling.");
    cli.add_flag("--gc-incremental", opts.gc_incremental, "Enable incremental GC collection.");
    cli.add_flag("--debug", opts.debug, "Enable debug symbol generation for generated code.");
    cli.add_flag("--direct-call",
                 opts.direct_call,
                 "Elides the dereferencing of vars for improved performance.");
    cli
      .add_option("-O,--optimization",
                  opts.optimization_level,
                  "The optimization level to use for AOT compilation.")
      /* TODO: This does not validate. */
      ->check(CLI::Range(0, 3));

    std::map<std::string, codegen_type> const codegen_types{
      {    "llvm-ir",    codegen_type::llvm_ir },
      {        "cpp",        codegen_type::cpp },
      {   "wasm-aot",   codegen_type::wasm_aot },
      { "wasm-patch", codegen_type::wasm_patch }
    };
    cli.add_option("--codegen", opts.codegen, "The type of code generation to use.")
      ->transform(
        CLI::CheckedTransformer(codegen_types).description("{llvm-ir,cpp,wasm-aot,wasm-patch}"))
      ->default_str(make_default(codegen_type_str(opts.codegen)));
    cli.add_flag("--save-cpp",
                 opts.save_cpp,
                 "Save generated C++ code to a file (useful for AOT/WASM compilation).");
    cli.add_option("--save-cpp-path",
                   opts.save_cpp_path,
                   "Path to save generated C++ code (requires --save-cpp or --codegen wasm-aot).");
    cli.add_flag("--save-llvm-ir",
                 opts.save_llvm_ir,
                 "Save generated LLVM IR to a file (useful for WASM/cross-compilation).");
    cli.add_option("--save-llvm-ir-path",
                   opts.save_llvm_ir_path,
                   "Path to save generated LLVM IR code.");

    /* Native dependencies. */
    cli.add_option("-I,--include-dir",
                   opts.include_dirs,
                   "Absolute or relative path to the directory for includes resolution. Can be "
                   "specified multiple times.");
    cli.add_option("-L,--library-dir",
                   opts.library_dirs,
                   "Absolute or relative path to the directory to search dynamic libraries in. "
                   "Can be specified multiple times.");
    cli.add_option("-D,--define-macro",
                   opts.define_macros,
                   "Defines macro value, sets to 1 if omitted. Can be specified multiple times.");
    cli.add_option("-l,--lib",
                   opts.libs,
                   "Library identifiers, absolute or relative paths eg. -lfoo for libfoo.so or "
                   "foo.dylib. Can be specified multiple times.");
    cli.add_option(
      "--jit-lib",
      opts.jit_libs,
      "Libraries to load into JIT only (not passed to AOT linker). "
      "Use for symbol resolution during compilation without creating runtime dependency. "
      "Can be specified multiple times.");
    cli.add_option("--link-lib",
                   opts.link_libs,
                   "Libraries to pass to AOT linker only (not loaded into JIT). "
                   "Use for static libraries (.a) or libraries only needed at link time. "
                   "Can be specified multiple times.");
    cli.add_option("--obj",
                   opts.object_files,
                   "Absolute or relative path to object files (.o) to load into JIT. "
                   "Can be specified multiple times.");
    cli.add_option("--framework",
                   opts.frameworks,
                   "macOS framework to link (e.g., --framework Cocoa). "
                   "Can be specified multiple times.");

    /* Run subcommand. */
    auto &cli_run(*cli.add_subcommand("run", "Load and run a file."));
    cli_run.fallthrough();
    cli_run.add_option("file", opts.target_file, "The entrypoint file.")
      ->check(CLI::ExistingFile)
      ->required();

    /* Compile module subcommand. */
    auto &cli_compile_module(
      *cli.add_subcommand("compile-module",
                          "Compile a module (given its namespace) and its dependencies."));
    cli_compile_module.fallthrough();
    cli_compile_module
      .add_option("--runtime", opts.target_runtime, "The runtime of the compiled program.")
      ->check(CLI::IsMember({ "dynamic", "static" }))
      ->default_str(make_default("dynamic"));
    cli_compile_module
      .add_option("module", opts.target_module, "Module to compile (must be on the module path).")
      ->required();
    cli_compile_module.add_option("-o", opts.output_object_filename, "Output object file name.");

    /* REPL subcommand. */
    auto &cli_repl(*cli.add_subcommand("repl", "Start up a terminal REPL and optional server."));
    cli_repl.fallthrough();
    cli_repl.add_option("module", opts.target_module, "The entrypoint module.");
    cli_repl.add_flag("--server", opts.repl_server, "Start an nREPL server.");

    /* C++ REPL subcommand. */
    auto &cli_cpp_repl(*cli.add_subcommand("cpp-repl", "Start up a terminal C++ REPL."));

    /* Run-main subcommand. */
    auto &cli_run_main(*cli.add_subcommand("run-main", "Load and execute -main."));
    cli_run_main.fallthrough();
    cli_run_main
      .add_option("module",
                  opts.target_module,
                  "The entrypoint module (must be on the module path.")
      ->required();

    /* Compile subcommand. */
    auto &cli_compile(*cli.add_subcommand(
      "compile",
      "Ahead of time compile project with entrypoint module containing -main."));
    cli_compile.fallthrough();
    cli_compile
      .add_option("--runtime", opts.target_runtime, "The runtime of the compiled program.")
      ->check(CLI::IsMember({ "dynamic", "static" }))
      ->default_str(make_default(opts.target_runtime));
    cli_compile.add_option("-o", opts.output_filename, "Output executable name.")
      ->default_str(make_default(opts.output_filename));
    cli_compile.add_option("module", opts.target_module, "The entrypoint module.")->required();

    /* Health check subcommand. */
    auto &cli_check_health(
      *cli.add_subcommand("check-health", "Provide a status report on the jank installation."));
    cli_check_health.fallthrough();

    cli.require_subcommand(1);
    cli.failure_message(CLI::FailureMessage::help);
    cli.allow_extras();

    try
    {
      cli.parse(argc, argv);
    }
    catch(CLI::ParseError const &e)
    {
      return err(cli.exit(e));
    }

    if(cli.remaining_size() >= 0)
    {
      opts.extra_opts = cli.remaining();
    }

    if(cli.got_subcommand(&cli_run))
    {
      opts.command = command::run;
    }
    else if(cli.got_subcommand(&cli_compile_module))
    {
      opts.command = command::compile_module;
    }
    else if(cli.got_subcommand(&cli_repl))
    {
      opts.command = command::repl;
    }
    else if(cli.got_subcommand(&cli_cpp_repl))
    {
      opts.command = command::cpp_repl;
    }
    else if(cli.got_subcommand(&cli_run_main))
    {
      opts.command = command::run_main;
    }
    else if(cli.got_subcommand(&cli_compile))
    {
      opts.command = command::compile;
    }
    else if(cli.got_subcommand(&cli_check_health))
    {
      opts.command = command::check_health;
    }

    /* --profile-core implies --profile-fns */
    if(opts.profiler_core_enabled)
    {
      opts.profiler_fns_enabled = true;
    }

    /* --profile-fns implies --profile */
    if(opts.profiler_fns_enabled)
    {
      opts.profiler_enabled = true;
    }

    /* --profile-interop implies --profile */
    if(opts.profiler_interop_enabled)
    {
      opts.profiler_enabled = true;
    }

    return ok();
  }

  std::vector<std::string> parse_empty(int const argc, char const **argv)
  {
    CLI::App cli{ "jank default cli" };
    cli.allow_extras();
    cli.parse(argc, argv);

    return cli.remaining();
  }
}
