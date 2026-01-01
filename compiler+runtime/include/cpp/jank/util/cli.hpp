#pragma once

#include <jtl/result.hpp>

namespace jank::util::cli
{
  enum class command : u8
  {
    run,
    compile,
    compile_module,
    repl,
    cpp_repl,
    run_main,
    check_health
  };

  enum class codegen_type : u8
  {
    llvm_ir,
    cpp,
    wasm_aot, // Generates standalone C++ for WASM AOT compilation
    wasm_patch // Generates SIDE_MODULE patches for hot-reload
  };

  constexpr char const *codegen_type_str(codegen_type const type)
  {
    switch(type)
    {
      case codegen_type::llvm_ir:
        return "llvm-ir";
      case codegen_type::cpp:
        return "cpp";
      case codegen_type::wasm_aot:
        return "wasm-aot";
      case codegen_type::wasm_patch:
        return "wasm-patch";
      default:
        return "unknown";
    }
  }

  enum class compilation_target : u8
  {
    /* The target will be determined based on the extension of the output.
     * If that's not possible, we'll error out. */
    unspecified,
    llvm_ir,
    cpp,
    object
  };

  constexpr char const *compilation_target_str(compilation_target const target)
  {
    switch(target)
    {
      case compilation_target::unspecified:
        return "unspecified";
      case compilation_target::llvm_ir:
        return "llvm-ir";
      case compilation_target::cpp:
        return "cpp";
      case compilation_target::object:
        return "object";
      default:
        return "unknown";
    }
  }

  struct options
  {
    /* Runtime. */
    jtl::immutable_string module_path;
    jtl::immutable_string profiler_file{ "jank.profile" };
    bool profiler_enabled{};
    bool profiler_fns_enabled{};
    bool profiler_core_enabled{}; /* Profile clojure.core functions */
    bool profiler_interop_enabled{}; /* Profile cpp/ interop (box/unbox/method calls) */
    u32 profiler_sample_rate{}; /* Sample 1 in N events (0=all, 10=10%, 100=1%, etc.) */
    bool perf_profiling_enabled{};
    bool gc_incremental{};
    bool jit_cache_enabled{ true }; /* Cache compiled defs to skip redundant JIT */
    codegen_type codegen{ codegen_type::cpp };

    /* Native dependencies. */
    native_vector<jtl::immutable_string> include_dirs;
    native_vector<jtl::immutable_string> library_dirs;
    native_vector<jtl::immutable_string> define_macros;
    native_vector<jtl::immutable_string> libs;
    native_vector<jtl::immutable_string> jit_libs; /* Libraries for JIT only (not AOT linker) */
    native_vector<jtl::immutable_string> link_libs; /* Libraries for AOT linker only (not JIT) */
    native_vector<jtl::immutable_string> object_files;
    native_vector<jtl::immutable_string> frameworks; /* macOS frameworks */

    /* Compilation. */
    bool debug{};
    u8 optimization_level{};
    bool direct_call{};
    bool save_cpp{};
    std::string save_cpp_path;
    bool save_llvm_ir{};
    std::string save_llvm_ir_path;

    /* Run command. */
    jtl::immutable_string target_file;

    /* Compile command. */
    jtl::immutable_string target_module;
    jtl::immutable_string target_runtime{ "dynamic" };
    jtl::immutable_string output_filename{ "a.out" };
    std::string output_object_filename;
    bool output_shared_library{};

    /* Compile-module command. */
    jtl::immutable_string output_module_filename;
    compilation_target output_target{ compilation_target::unspecified };
    bool list_modules{}; /* Print loaded modules in dependency order (for AOT build scripts) */

    /* REPL command. */
    bool repl_server{};

    /* iOS compile server (for run-main command). */
    uint16_t ios_compile_server_port{}; /* 0 = disabled, otherwise port number */
    std::string ios_resource_dir{}; /* Path to iOS resources (PCH, headers) */

    /* Extra flags, which will be passed to main. */
    native_vector<jtl::immutable_string> extra_opts;

    command command{ command::repl };
  };

  /* NOLINTNEXTLINE */
  extern options opts;

  /* Affects the global opts. */
  jtl::result<void, int> parse_opts(int const argc, char const **argv);

  /* Takes the CLI args and puts 'em in a vector. */
  native_vector<jtl::immutable_string> parse_into_vector(int const argc, char const **argv);
}
