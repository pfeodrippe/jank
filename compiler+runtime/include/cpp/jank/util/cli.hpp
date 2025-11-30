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

  struct options
  {
    /* Runtime. */
    std::string module_path;
    std::string profiler_file{ "jank.profile" };
    bool profiler_enabled{};
    bool perf_profiling_enabled{};
    bool gc_incremental{};
    codegen_type codegen{ codegen_type::cpp };

    /* Native dependencies. */
    native_vector<jtl::immutable_string> include_dirs;
    native_vector<jtl::immutable_string> library_dirs;
    native_vector<jtl::immutable_string> define_macros;
    native_vector<jtl::immutable_string> libs;
    native_vector<jtl::immutable_string> object_files;

    /* Compilation. */
    bool debug{};
    u8 optimization_level{};
    bool direct_call{};
    bool save_cpp{};
    std::string save_cpp_path;
    bool save_llvm_ir{};
    std::string save_llvm_ir_path;

    /* Run command. */
    std::string target_file;

    /* Compile command. */
    std::string target_module;
    std::string target_runtime{ "dynamic" };
    std::string output_filename{ "a.out" };
    std::string output_object_filename;
    bool output_shared_library{};

    /* REPL command. */
    bool repl_server{};

    /* Extras.
     * TODO: Use a native_persistent_vector instead.
     * */
    std::vector<std::string> extra_opts;

    command command{ command::repl };
  };

  /* NOLINTNEXTLINE */
  extern options opts;

  jtl::result<void, int> parse(int const argc, char const **argv);
  std::vector<std::string> parse_empty(int const argc, char const **argv);
}
