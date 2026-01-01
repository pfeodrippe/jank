#pragma once

#include <filesystem>
#include <memory>
#include <csetjmp>
#include <csignal>
#include <unordered_set>

#include <jtl/option.hpp>
#include <jtl/result.hpp>
#include <jtl/string_builder.hpp>

namespace llvm
{
  class Module;
  class LLVMContext;

  namespace orc
  {
    class ThreadSafeModule;
  }
}

namespace clang
{
  class Value;
}

namespace Cpp
{
  class Interpreter;
}

namespace jank::jit
{
  /* Result struct for eval_string_with_result, providing access to
   * the C++ interpreter's expression result similar to clang-repl output. */
  struct eval_result
  {
    /* Whether the evaluation succeeded and produced a valid result. */
    bool valid{};

    /* Whether the result is void (valid but no value). */
    bool is_void{};

    /* Raw pointer to the result value (if applicable). */
    void *ptr{};

    /* The C++ type as a string, e.g. "std::vector<int> &". */
    jtl::immutable_string type_str;

    /* The printed representation of the value, similar to clang-repl output. */
    jtl::immutable_string repr;
  };

  /* Thread-local recovery point for fatal LLVM errors. This allows callers (like nREPL eval)
   * to register a jmp_buf that the fatal error handler can use to recover instead of exiting.
   * When set, LLVM fatal errors will longjmp to this point with the JIT_FATAL_ERROR_SIGNAL. */
  constexpr int JIT_FATAL_ERROR_SIGNAL = 99;

  /* Register a recovery point for the current thread. Pass nullptr to clear. */
  void set_jit_recovery_point(jmp_buf *buf);

  /* Get the current thread's recovery point, or nullptr if none. */
  jmp_buf *get_jit_recovery_point();

  /* RAII guard for setting/clearing the recovery point. */
  struct scoped_jit_recovery
  {
    explicit scoped_jit_recovery(jmp_buf &buf);
    ~scoped_jit_recovery();

    scoped_jit_recovery(scoped_jit_recovery const &) = delete;
    scoped_jit_recovery &operator=(scoped_jit_recovery const &) = delete;
  };

  struct processor
  {
    processor(jtl::immutable_string const &binary_version);
    ~processor();

    void eval_string(jtl::immutable_string const &s) const;

    /* Evaluates a C++ expression and returns detailed result information including
     * the type and printed representation, similar to clang-repl output.
     * Note: The expression should NOT end with a semicolon to capture the result. */
    jtl::result<eval_result, jtl::immutable_string>
    eval_string_with_result(jtl::immutable_string const &s) const;

    void eval_string(jtl::immutable_string const &s, clang::Value *) const;
    void load_object(jtl::immutable_string_view const &path) const;
    bool load_object(char const *data, size_t size, std::string const &name) const;
    jtl::string_result<void> load_dynamic_library(jtl::immutable_string const &path) const;
    void load_ir_module(llvm::orc::ThreadSafeModule &&m) const;
    void load_bitcode(jtl::immutable_string const &module,
                      jtl::immutable_string_view const &bitcode) const;

    jtl::string_result<void> remove_symbol(jtl::immutable_string const &name) const;
    jtl::string_result<void *> find_symbol(jtl::immutable_string const &name) const;

    jtl::result<void, jtl::immutable_string>
    load_dynamic_libs(native_vector<jtl::immutable_string> const &libs) const;
    jtl::option<jtl::immutable_string> find_dynamic_lib(jtl::immutable_string const &lib) const;

    std::unique_ptr<Cpp::Interpreter> interpreter;
    native_vector<std::filesystem::path> library_dirs;

    /* The files within this map will get added into Clang's VFS prior to the creation of
     * the `clang::Interpreter`. This allows us to embed the PCH into AOT compiled programs
     * while still being able to include it. */
    std::map<char const *, std::string_view> vfs;

    /* Tracks loaded object files to make load_object idempotent. This prevents
     * LLVM errors when CIDER/nREPL's load-file re-evaluates files that call load_object. */
    mutable std::unordered_set<std::string> loaded_objects_;
  };
}
