#include <clang/AST/Type.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Interpreter/Value.h>
#include <Interpreter/Compatibility.h>
#include <clang/Interpreter/CppInterOp.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Signals.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Debugging/PerfSupportPlugin.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebugInfoSupport.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderPerf.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/IRReader/IRReader.h>

#include <jank/util/cpptrace.hpp>

#include <jank/jit/processor.hpp>
#include <jank/util/make_array.hpp>
#include <jank/util/environment.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/clang.hpp>
#include <jank/util/clang_format.hpp>
#include <jank/runtime/context.hpp>
#include <jank/profile/time.hpp>
#include <jank/error/system.hpp>

namespace jank::jit
{
  /* Thread-local recovery point for LLVM fatal errors. */
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static thread_local jmp_buf *jit_recovery_buf{ nullptr };

  void set_jit_recovery_point(jmp_buf *buf)
  {
    jit_recovery_buf = buf;
  }

  jmp_buf *get_jit_recovery_point()
  {
    return jit_recovery_buf;
  }

  scoped_jit_recovery::scoped_jit_recovery(jmp_buf &buf)
  {
    set_jit_recovery_point(&buf);
  }

  scoped_jit_recovery::~scoped_jit_recovery()
  {
    set_jit_recovery_point(nullptr);
  }

  static jtl::immutable_string default_shared_lib_name(jtl::immutable_string const &lib)
#if defined(__APPLE__)
  {
    return util::format("{}.dylib", lib);
  }
#elif defined(__linux__)
  {
    return util::format("lib{}.so", lib);
  }
#endif

  static void handle_fatal_llvm_error(void * const, char const *message, bool const gen_crash_diag)
  {
    /* Log the error message to stderr. */
    std::cerr << "LLVM fatal error: " << (message ? message : "(null)") << "\n";

    /* Run the interrupt handlers to make sure any special cleanups get done, in
       particular that we remove files registered with RemoveFileOnSignal. */
    llvm::sys::RunInterruptHandlers();

    /* Check if a recovery point has been registered (e.g., by nREPL eval).
     * If so, longjmp to it instead of exiting. This allows the caller to
     * handle the error gracefully. */
    if(auto *buf = get_jit_recovery_point())
    {
      // NOLINTNEXTLINE(modernize-avoid-setjmp-longjmp)
      longjmp(*buf, JIT_FATAL_ERROR_SIGNAL);
    }

    /* No recovery point - exit as before. */
    std::exit(gen_crash_diag ? 70 : 1);
  }

  /* LLVM will register JIT compiled frames for GDB/LLDB using a standard
   * interface which is described here:
   *
   * https://weliveindetail.github.io/blog/post/2022/11/27/gdb-jit-interface-101.html
   *
   * The debuggers implicitly place breakpoints on the `__jit_debug_register_code`
   * function, which is called as part of LLVM's registration. This is an empty
   * function, but the breakpoint triggering tells the debugger to update its
   * entries based on the `__jit_debug_descriptor` linked list.
   *
   * Here, we manually trigger the same thing, to have cpptrace update its
   * view of the available JIT compiler frames. We do this after loading any
   * new JIT compiled code. */
  static void register_jit_stack_frames()
  {
    if(auto *entry = cpptrace::detail::__jit_debug_descriptor.relevant_entry)
    {
      cpptrace::register_jit_object(entry->symfile_addr, entry->symfile_size);
    }
  }

  processor::processor(jtl::immutable_string const &binary_version)
  {
    profile::timer const timer{ "jit ctor" };

    for(auto const &library_dir : util::cli::opts.library_dirs)
    {
      library_dirs.emplace_back(std::filesystem::absolute(library_dir.c_str()));
    }

    /* When we AOT compile the jank compiler/runtime, we keep track of the compiler
     * flags used so we can use the same set during JIT compilation. Here we parse these
     * into a vector for Clang. Since Clang wants a vector<char const*>, we need to
     * dynamically allocate. These will never be freed. */
    std::vector<char const *> args{};
    std::stringstream flags{ JANK_JIT_FLAGS };
    std::string flag;
    while(std::getline(flags, flag, ' '))
    {
      args.emplace_back(strdup(flag.c_str()));
    }

    if(auto const extra{ getenv("JANK_EXTRA_FLAGS") }; extra)
    {
      std::stringstream flags{ extra };
      std::string flag;
      while(std::getline(flags, flag, ' '))
      {
        args.emplace_back(strdup(flag.c_str()));
      }
    }

    if(util::cli::opts.debug || util::cli::opts.perf_profiling_enabled)
    {
      args.emplace_back("-g");
    }

    auto const clang_path_str{ util::find_clang() };
    if(clang_path_str.is_none())
    {
      throw error::system_clang_executable_not_found();
    }
    auto const clang_dir{ std::filesystem::path{ clang_path_str.unwrap().c_str() }.parent_path() };
    args.emplace_back("-I");
    args.emplace_back(strdup((clang_dir / "../include").c_str()));

    auto const clang_resource_dir{ util::find_clang_resource_dir() };
    if(clang_resource_dir.is_none())
    {
      throw error::system_failure(
        util::format("Unable to find Clang {} resource dir.", JANK_CLANG_MAJOR_VERSION));
    }
    args.emplace_back("-resource-dir");
    args.emplace_back(clang_resource_dir.unwrap().c_str());

    auto const jank_resource_dir{ util::resource_dir() };
    args.emplace_back("-I");
    args.emplace_back(strdup(util::format("{}/include", jank_resource_dir).c_str()));

    args.emplace_back("-L");
    args.emplace_back(strdup(util::format("{}/lib", jank_resource_dir).c_str()));

    /* We need to include our special runtime PCH. */
#if !defined(JANK_TARGET_IOS)
    /* On desktop, build PCH on demand if not found. */
    auto pch_path{ util::find_pch(binary_version) };
    if(pch_path.is_none())
    {
      auto const res{ util::build_pch(args, binary_version) };
      if(res.is_err())
      {
        throw res.expect_err();
      }
      pch_path = res.expect_ok();
    }
    auto const &pch_path_str{ pch_path.unwrap() };
    args.emplace_back("-include-pch");
    args.emplace_back(strdup(pch_path_str.c_str()));
#else
    /* On iOS, PCH is optional - if it exists (pre-bundled), use it.
     * Otherwise we'll parse the prelude header after interpreter creation. */
    auto pch_path{ util::find_pch(binary_version) };
    bool const has_pch{ pch_path.is_some() };
    if(has_pch)
    {
      auto const &pch_path_str{ pch_path.unwrap() };
      args.emplace_back("-include-pch");
      args.emplace_back(strdup(pch_path_str.c_str()));
    }
#endif

    args.emplace_back("-w");
    args.emplace_back("-Wno-c++11-narrowing");

    util::add_system_flags(args);

    /********* Every flag after this line is user-provided. *********/

    for(auto const &include_path : util::cli::opts.include_dirs)
    {
      args.emplace_back(strdup(util::format("-I{}", include_path).c_str()));
    }

    for(auto const &library_path : util::cli::opts.library_dirs)
    {
      args.emplace_back(strdup(util::format("-L{}", library_path).c_str()));
    }

    for(auto const &define_macro : util::cli::opts.define_macros)
    {
      args.emplace_back(strdup(util::format("-D{}", define_macro).c_str()));
    }

    //util::println("jit flags {}", args);

    interpreter.reset(static_cast<Cpp::Interpreter *>(
      Cpp::CreateInterpreter(args, {}, vfs, static_cast<int>(llvm::CodeModel::Large))));

#if defined(JANK_TARGET_IOS)
    /* On iOS without PCH, we need to explicitly include the prelude header
     * so that jank types are available for the analyzer. This is slower than
     * using a pre-compiled header, but ensures types are properly registered. */
    if(!has_pch)
    {
      profile::timer const pch_timer{ "jit prelude parse" };
      auto result = interpreter->process("#include <jank/prelude.hpp>");
      if(result != Cpp::Interpreter::kSuccess)
      {
        throw std::runtime_error("Failed to parse jank prelude header");
      }
    }
#endif

    /* Install our custom fatal error handler so we can recover from LLVM crashes in the REPL.
     * The handler checks if a recovery point has been registered (via set_jit_recovery_point)
     * and longjmps to it instead of calling exit(). */
    llvm::install_fatal_error_handler(handle_fatal_llvm_error, nullptr);

    /* Enabling perf support requires registering a couple of plugins with LLVM. These
     * plugins will generate files which perf can then use to inject additional info
     * into its recorded data (via `perf inject`).
     *
     * Note that we need to manually get the start/end/impl address for perf, rather than
     * using the PerfSupportPlugin::Create factory, since the latter leads to crashes on
     * Clang 19, at least. This workaround was suggested by and borrowed from Julia devs.
     *
     * https://github.com/mortenpi/julia/blob/1edc6f1b7752ed67059020ba7ce174dffa225954/src/jitlayers.cpp#L2330
     *
     * Perf profiling is not available on iOS.
     */
#if !defined(JANK_TARGET_IOS)
    if(util::cli::opts.perf_profiling_enabled)
    {
      auto const ee{ interpreter->getExecutionEngine() };
      auto &es{ ee->getExecutionSession() };
      auto &ol{ ee->getObjLinkingLayer() };
      auto &oll{ llvm::cast<llvm::orc::ObjectLinkingLayer>(ol) };

#define add_address_to_map(map, name)                                     \
  ((map)[es.intern(ee->mangle(#name))]                                    \
   = { llvm::orc::ExecutorAddr::fromPtr(&(name)),                         \
       llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable }, \
   llvm::orc::ExecutorAddr::fromPtr(&(name)))

      llvm::orc::SymbolMap perf_fns;
      auto const start_addr{ add_address_to_map(perf_fns, llvm_orc_registerJITLoaderPerfStart) };
      auto const end_addr{ add_address_to_map(perf_fns, llvm_orc_registerJITLoaderPerfEnd) };
      auto const impl_addr{ add_address_to_map(perf_fns, llvm_orc_registerJITLoaderPerfImpl) };
      llvm::cantFail(ee->getMainJITDylib().define(llvm::orc::absoluteSymbols(perf_fns)));
      oll.addPlugin(llvm::cantFail(llvm::orc::DebugInfoPreservationPlugin::Create()));
      oll.addPlugin(std::make_unique<llvm::orc::PerfSupportPlugin>(es.getExecutorProcessControl(),
                                                                   start_addr,
                                                                   end_addr,
                                                                   impl_addr,
                                                                   true,
                                                                   true));
    }
#endif

    auto const &load_result{ load_dynamic_libs(util::cli::opts.libs) };
    if(load_result.is_err())
    {
      throw error::system_failure(load_result.expect_err().c_str());
    }

    /* Load JIT-only libraries from --jit-lib CLI option.
     * These are loaded for symbol resolution but not passed to AOT linker. */
    auto const &jit_load_result{ load_dynamic_libs(util::cli::opts.jit_libs) };
    if(jit_load_result.is_err())
    {
      throw error::system_failure(jit_load_result.expect_err().c_str());
    }

    /* Load object files from --obj CLI option. */
    for(auto const &obj_path : util::cli::opts.object_files)
    {
      load_object(obj_path);
    }
  }

  processor::~processor()
  {
    llvm::remove_fatal_error_handler();
  }

  void processor::eval_string(jtl::immutable_string const &s) const
  {
    profile::timer const timer{ "jit eval_string" };
    auto const &formatted{ s };
    //auto const &formatted{ util::format_cpp_source(s).expect_ok() };
    //util::println("// eval_string:\n{}\n", formatted);
    auto err(interpreter->ParseAndExecute({ formatted.data(), formatted.size() }));
    if(err)
    {
      llvm::logAllUnhandledErrors(std::move(err), llvm::errs(), "error: ");
      llvm::errs().flush();
      /* Include the code in the error message for better debugging */
      auto const preview_len{ std::min<size_t>(formatted.size(), 500) };
      native_transient_string code_preview{ formatted.data(), preview_len };
      if(formatted.size() > 500)
      {
        code_preview.append("...(truncated)");
      }
      throw std::runtime_error{ util::format("Failed to evaluate C++ code:\n{}", code_preview) };
    }
    register_jit_stack_frames();
  }

  jtl::result<eval_result, jtl::immutable_string>
  processor::eval_string_with_result(jtl::immutable_string const &s) const
  {
    profile::timer const timer{ "jit eval_string_with_result" };

    clang::Value v;
    auto parse_err(interpreter->ParseAndExecute({ s.data(), s.size() }, &v));
    if(parse_err)
    {
      std::string err_msg;
      llvm::raw_string_ostream err_stream{ err_msg };
      llvm::logAllUnhandledErrors(std::move(parse_err), err_stream, "");
      err_stream.flush();

      auto const preview_len{ std::min<size_t>(s.size(), 200) };
      native_transient_string code_preview{ s.data(), preview_len };
      if(s.size() > 200)
      {
        code_preview.append("...");
      }

      return err(util::format("Failed to evaluate C++ expression '{}': {}",
                              code_preview,
                              jtl::immutable_string{ err_msg.data(), err_msg.size() }));
    }

    register_jit_stack_frames();

    eval_result result;
    result.valid = v.isValid();
    result.is_void = v.isVoid();

    if(result.valid && !result.is_void)
    {
      result.ptr = v.getPtr();

      /* Capture the type string */
      {
        std::string type_buf;
        llvm::raw_string_ostream type_stream{ type_buf };
        v.printType(type_stream);
        type_stream.flush();
        result.type_str = jtl::immutable_string{ type_buf.data(), type_buf.size() };
      }

      /* Capture the full printed representation (type + data) */
      {
        std::string repr_buf;
        llvm::raw_string_ostream repr_stream{ repr_buf };
        v.print(repr_stream);
        repr_stream.flush();
        result.repr = jtl::immutable_string{ repr_buf.data(), repr_buf.size() };
      }
    }
    else if(result.is_void)
    {
      result.type_str = "void";
      result.repr = "(void)";
    }

    return ok(jtl::move(result));
  }

  void processor::load_object(jtl::immutable_string_view const &path) const
  {
    /* Canonicalize the path to detect duplicates even with different relative paths. */
    std::error_code ec;
    auto const canonical_path{ std::filesystem::canonical(std::string_view{ path }, ec) };
    std::string const path_key{ ec ? std::string{ path } : canonical_path.string() };

    /* Skip if already loaded - makes this idempotent for nREPL load-file operations. */
    if(loaded_objects_.contains(path_key))
    {
      return;
    }

    auto const ee{ interpreter->getExecutionEngine() };
    auto file{ llvm::MemoryBuffer::getFile(std::string_view{ path }) };
    if(!file)
    {
      throw std::runtime_error{ util::format("failed to load object file: {}", path) };
    }
    /* XXX: Object files won't be able to use global ctors until jank is on the ORC
     * runtime, which likely won't happen until clang::Interpreter is on the ORC runtime. */
    /* TODO: Return result on failure. */
    llvm::cantFail(ee->addObjectFile(std::move(file.get())));
    loaded_objects_.insert(path_key);
    register_jit_stack_frames();
  }

  void processor::load_ir_module(llvm::orc::ThreadSafeModule &&m) const
  {
    auto const &module_name{ m.getModuleUnlocked()->getName() };
    profile::timer const timer{ util::format(
      "jit ir module {}",
      jtl::immutable_string_view{ module_name.data(), module_name.size() }) };
    //m->print(llvm::outs(), nullptr);

    auto const ee(interpreter->getExecutionEngine());
    llvm::cantFail(ee->addIRModule(jtl::move(m)));
    llvm::cantFail(ee->initialize(ee->getMainJITDylib()));
    register_jit_stack_frames();
  }

  void processor::load_bitcode(jtl::immutable_string const &module,
                               jtl::immutable_string_view const &bitcode) const
  {
    auto ctx{ std::make_unique<llvm::LLVMContext>() };
    llvm::SMDiagnostic err{};
    llvm::MemoryBufferRef const buf{
      std::string_view{ bitcode.data(), bitcode.size() },
      module.c_str()
    };
    auto ir_module{ llvm::parseIR(buf, err, *ctx) };
    if(!ir_module)
    {
      err.print("jank", llvm::errs());
      llvm::errs().flush();
      /* TODO: Return a result. */
      throw std::runtime_error{ util::format("unable to load module") };
    }
    load_ir_module({ std::move(ir_module), std::move(ctx) });
  }

  jtl::string_result<void> processor::remove_symbol(jtl::immutable_string const &name) const
  {
    auto const ee{ interpreter->getExecutionEngine() };
    llvm::orc::SymbolNameSet to_remove{};
    to_remove.insert(ee->mangleAndIntern(name.c_str()));
    auto const error{ ee->getMainJITDylib().remove(to_remove) };

    if(error.isA<llvm::orc::SymbolsCouldNotBeRemoved>())
    {
      return err(util::format("Failed to remove the symbol: '{}'", name));
    }
    return ok();
  }

  jtl::string_result<void *> processor::find_symbol(jtl::immutable_string const &name) const
  {
    if(auto symbol{ interpreter->getSymbolAddress(name.c_str()) })
    {
      return symbol.get().toPtr<void *>();
    }

    return err(util::format("Failed to find symbol: '{}'", name));
  }

  jtl::option<jtl::immutable_string>
  processor::find_dynamic_lib(jtl::immutable_string const &lib) const
  {
    auto const &default_lib_name{ default_shared_lib_name(lib) };
    for(auto const &lib_dir : library_dirs)
    {
      auto default_lib_abs_path{ util::format("{}/{}", lib_dir.string(), default_lib_name) };
      if(std::filesystem::exists(default_lib_abs_path.c_str()))
      {
        return default_lib_abs_path;
      }
      else
      {
        auto lib_abs_path{ util::format("{}/{}", lib_dir.string(), lib) };
        if(std::filesystem::exists(lib_abs_path.c_str()))
        {
          return lib_abs_path;
        }
      }
    }

    return none;
  }

  jtl::result<void, jtl::immutable_string>
  processor::load_dynamic_libs(native_vector<jtl::immutable_string> const &libs) const
  {
    for(auto const &lib : libs)
    {
      if(std::filesystem::path{ lib.c_str() }.is_absolute())
      {
        auto const load_res{ load_dynamic_library(lib) };
        if(load_res.is_err())
        {
          return err(load_res.expect_err());
        }
        continue;
      }

      auto const result{ processor::find_dynamic_lib(lib) };
      if(result.is_some())
      {
        auto const load_res{ load_dynamic_library(result.unwrap()) };
        if(load_res.is_err())
        {
          return err(load_res.expect_err());
        }
        continue;
      }

      auto const &default_lib_name{ default_shared_lib_name(lib) };
      auto const load_default_res{ load_dynamic_library(default_lib_name) };
      if(load_default_res.is_ok())
      {
        continue;
      }

      auto const load_raw_res{ load_dynamic_library(lib) };
      if(load_raw_res.is_ok())
      {
        continue;
      }

      return err(load_raw_res.expect_err());
    }

    return ok();
  }

  jtl::string_result<void> processor::load_dynamic_library(jtl::immutable_string const &path) const
  {
    if(path.empty())
    {
      return err("Attempted to load an empty library path.");
    }

    auto load_err{
      static_cast<clang::Interpreter &>(*interpreter).LoadDynamicLibrary(path.data())
    };
    if(load_err)
    {
      std::string err_message;
      llvm::handleAllErrors(std::move(load_err),
                            [&](llvm::ErrorInfoBase &info) { err_message = info.message(); });
      if(err_message.empty())
      {
        err_message = "unknown error";
      }

      return err(util::format("Failed to load dynamic library '{}': {}", path, err_message));
    }

    return ok();
  }
}
