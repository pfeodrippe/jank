#pragma once

// macOS Compilation Server for iOS Remote JIT
//
// This server receives jank code from iOS, compiles it to ARM64 object files,
// and sends them back for execution. This avoids the symbol duplication problem
// that occurs when CppInterOp parses C++ headers on iOS.
//
// Architecture:
//   iOS nREPL ─────► macOS Compile Server ─────► iOS loads object
//        (code)              (ARM64 .o)              (execute)
//
// The server uses jank's normal compilation pipeline but cross-compiles to iOS.

#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <cxxabi.h>
#include <cstdlib>
#include <pthread.h>

#include <gc/gc.h>
// Forward declarations for GC thread registration (may not be exposed in all gc.h versions)
extern "C" {
GC_API void GC_CALL GC_allow_register_threads(void);
GC_API int GC_CALL GC_pthread_create(pthread_t *, const pthread_attr_t *,
                                      void *(*)(void *), void *);
}

#include <boost/asio.hpp>

#include <jank/compile_server/protocol.hpp>
#include <jank/runtime/context.hpp>
#include <jank/util/environment.hpp>
#include <jank/util/cli.hpp>
#include <jank/runtime/core/munge.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/obj/persistent_list.hpp>
#include <jank/runtime/obj/symbol.hpp>
#include <jank/runtime/module/loader.hpp>
#include <jank/read/lex.hpp>
#include <jank/read/parse.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/analyze/pass/optimize.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/evaluate.hpp>
#include <jank/error.hpp>
#include <jank/util/fmt.hpp>

namespace jank::compile_server
{
  using boost::asio::ip::tcp;

  // Configuration for cross-compilation
  struct ios_compile_config
  {
    std::string clang_path;           // Path to clang for cross-compilation
    std::string ios_sdk_path;         // Path to iOS SDK (iPhoneSimulator or iPhoneOS)
    std::string pch_path;             // Path to iOS PCH file
    std::vector<std::string> include_paths;  // Additional include paths
    std::vector<std::string> flags;          // Additional compiler flags
    std::string target_triple;        // e.g., "arm64-apple-ios17.0-simulator"
    std::string temp_dir;             // Temp directory for generated files
  };

  class server
  {
  public:
    server(uint16_t port, ios_compile_config config)
      : port_(port)
      , config_(std::move(config))
      , acceptor_(io_context_)
      , running_(false)
      , server_thread_()
    {
      // Ensure temp directory exists
      std::filesystem::create_directories(config_.temp_dir);
    }

    ~server()
    {
      stop();
    }

    void start()
    {
      if(running_.exchange(true))
      {
        return;
      }

      try
      {
        tcp::endpoint endpoint(tcp::v4(), port_);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();

        std::cout << "[compile-server] Started on port " << port_ << std::endl;
        std::cout << "[compile-server] Target: " << config_.target_triple << std::endl;
        std::cout << "[compile-server] SDK: " << config_.ios_sdk_path << std::endl;

        // Start server thread with GC registration
        // Must register with Boehm GC since this thread will allocate GC memory during eval
        GC_allow_register_threads();

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // Set stack size to 16MB (needed for complex C++ headers)
        constexpr size_t LARGE_STACK_SIZE = static_cast<size_t>(16) * 1024 * 1024;
        pthread_attr_setstacksize(&attr, LARGE_STACK_SIZE);

        /* Note: We explicitly call GC_pthread_create (not pthread_create) to ensure
         * the thread is properly registered with Boehm GC for garbage collection. */
        auto thread_func = [](void *arg) -> void * {
          auto *self = static_cast<server *>(arg);
          /* Set up thread bindings for dynamic vars like *ns*.
           * This is required because eval operations may call (in-ns ...) which
           * needs *ns* to be thread-bound. */
          jank::runtime::context::binding_scope bindings;
          self->run();
          return nullptr;
        };

        GC_pthread_create(&server_thread_, &attr, thread_func, this);
        pthread_attr_destroy(&attr);
      }
      catch(std::exception const &e)
      {
        std::cerr << "[compile-server] Failed to start: " << e.what() << std::endl;
        running_ = false;
      }
    }

    void stop()
    {
      if(!running_.exchange(false))
      {
        return;
      }

      boost::system::error_code ec;
      acceptor_.close(ec);
      io_context_.stop();

      pthread_join(server_thread_, nullptr);

      std::cout << "[compile-server] Stopped." << std::endl;
    }

    bool is_running() const
    {
      return running_;
    }

  private:
    void run()
    {
      while(running_)
      {
        try
        {
          tcp::socket socket(io_context_);
          acceptor_.accept(socket);

          auto endpoint = socket.remote_endpoint();
          std::cout << "[compile-server] Client connected: " << endpoint.address().to_string()
                    << ":" << endpoint.port() << std::endl;

          handle_connection(std::move(socket));
        }
        catch(boost::system::system_error const &e)
        {
          if(running_ && e.code() != boost::asio::error::operation_aborted)
          {
            std::cerr << "[compile-server] Accept error: " << e.what() << std::endl;
          }
        }
      }
    }

    void handle_connection(tcp::socket socket)
    {
      try
      {
        boost::asio::streambuf buffer;
        std::string line;

        while(running_)
        {
          boost::system::error_code ec;
          boost::asio::read_until(socket, buffer, '\n', ec);

          if(ec)
          {
            if(ec != boost::asio::error::eof)
            {
              std::cerr << "[compile-server] Read error: " << ec.message() << std::endl;
            }
            break;
          }

          std::istream is(&buffer);
          std::getline(is, line);

          if(line.empty())
          {
            continue;
          }

          // Remove trailing \r if present
          if(!line.empty() && line.back() == '\r')
          {
            line.pop_back();
          }

          std::string response = handle_message(line) + "\n";
          boost::asio::write(socket, boost::asio::buffer(response), ec);

          if(ec)
          {
            std::cerr << "[compile-server] Write error: " << ec.message() << std::endl;
            break;
          }
        }
      }
      catch(std::exception const &e)
      {
        std::cerr << "[compile-server] Connection error: " << e.what() << std::endl;
      }

      std::cout << "[compile-server] Client disconnected." << std::endl;
    }

    std::string handle_message(std::string const &msg)
    {
      auto op = get_json_string(msg, "op");
      auto id = get_json_int(msg, "id");

      if(op == "compile")
      {
        auto code = get_json_string(msg, "code");
        auto ns = get_json_string(msg, "ns");
        auto module = get_json_string(msg, "module");

        if(code.empty())
        {
          return error_response(id, "Missing 'code' field", "protocol");
        }

        return compile_code(id, code, ns.empty() ? "user" : ns, module);
      }
      else if(op == "require")
      {
        auto ns = get_json_string(msg, "ns");
        auto source = get_json_string(msg, "source");

        if(ns.empty())
        {
          return error_response(id, "Missing 'ns' field", "protocol");
        }
        if(source.empty())
        {
          return error_response(id, "Missing 'source' field", "protocol");
        }

        return require_ns(id, ns, source);
      }
      else if(op == "ping")
      {
        return R"({"op":"pong","id":)" + std::to_string(id) + "}";
      }
      else
      {
        return error_response(id, "Unknown op: " + op, "protocol");
      }
    }

    std::string compile_code(int64_t id, std::string const &code, std::string const &ns,
                             std::string const &module_hint)
    {
      try
      {
        std::cout << "[compile-server] Compiling code (id=" << id << "): "
                  << code.substr(0, 50) << (code.size() > 50 ? "..." : "") << std::endl;

        // Step 1: Parse and analyze the jank code
        read::lex::processor l_prc{ code };
        read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

        analyze::processor an_prc;
        native_vector<analyze::expression_ref> exprs;

        for(auto const &form : p_prc)
        {
          auto const parsed_form = form.expect_ok().unwrap().ptr;
          auto expr = an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok();
          expr = analyze::pass::optimize(expr);
          exprs.push_back(expr);
        }

        if(exprs.empty())
        {
          return error_response(id, "No expressions to compile", "compile");
        }

        // Step 2: Wrap expressions in a function for codegen
        jtl::immutable_string module_name;
        if(module_hint.empty())
        {
          module_name = runtime::module::nest_module(ns, runtime::munge(runtime::__rt_ctx->unique_munged_string("repl")));
        }
        else
        {
          module_name = module_hint;
        }

        auto const fn_expr = evaluate::wrap_expressions(exprs, an_prc,
                                                        runtime::__rt_ctx->unique_munged_string("repl_fn"));

        // Step 3: Generate C++ code
        codegen::processor cg_prc{ fn_expr, module_name, codegen::compilation_target::eval };
        auto const cpp_code_body = cg_prc.declaration_str();
        auto const munged_struct_name = std::string(runtime::munge(cg_prc.struct_name.name).data());
        auto const entry_symbol = "_" + munged_struct_name + "_0";

        // Get the fully qualified struct name for the factory
        auto const module_ns = runtime::module::module_to_native_ns(module_name);
        auto const qualified_struct = std::string(module_ns.data(), module_ns.size()) +
                                      "::" + munged_struct_name;

        // Build complete C++ code with:
        // 1. Prelude include
        // 2. Native header includes (for aliases like sdfx::)
        // 3. Struct declaration (from codegen)
        // 4. Extern "C" factory function that creates and calls the struct
        std::string cpp_code;
        cpp_code += "#include <jank/prelude.hpp>\n";

        // Add native header includes from current namespace
        auto const current_ns = runtime::__rt_ctx->current_ns();
        auto const native_aliases = current_ns->native_aliases_snapshot();
        for(auto const &alias : native_aliases)
        {
          cpp_code += "#include <" + std::string(alias.header.data(), alias.header.size()) + ">\n";
        }
        cpp_code += "\n";

        cpp_code += std::string(cpp_code_body);
        cpp_code += "\n\n";
        cpp_code += "extern \"C\" ::jank::runtime::object_ref " + entry_symbol + "() {\n";
        cpp_code += "  return ::jank::runtime::make_box<" + qualified_struct + ">()->call();\n";
        cpp_code += "}\n";

        // Step 4: Cross-compile to ARM64 object file
        auto const object_result = cross_compile(id, cpp_code);

        if(!object_result.success)
        {
          return error_response(id, object_result.error, "cross-compile");
        }

        // Step 5: Return compiled object (base64 encoded)
        auto const encoded = base64_encode(object_result.object_data);

        return R"({"op":"compiled","id":)" + std::to_string(id)
          + R"(,"symbol":")" + escape_json(entry_symbol)
          + R"(","object":")" + encoded + R"("})";
      }
      catch(jtl::ref<error::base> const &e)
      {
        std::string msg = std::string(error::kind_str(e->kind)) + ": "
                        + std::string(e->message.data(), e->message.size());
        return error_response(id, msg, "compile");
      }
      catch(std::exception const &e)
      {
        return error_response(id, e.what(), "compile");
      }
      catch(...)
      {
        return error_response(id, "Unknown compilation error", "compile");
      }
    }

    // Require (load) a namespace - compile full namespace source AND transitive dependencies
    std::string require_ns(int64_t id, std::string const &ns_name, std::string const &source)
    {
      try
      {
        std::cout << "[compile-server] Requiring namespace (id=" << id << "): " << ns_name << std::endl;

        // Check if already loaded
        if(loaded_namespaces_.find(ns_name) != loaded_namespaces_.end())
        {
          std::cout << "[compile-server] Namespace already loaded: " << ns_name << std::endl;
          return R"({"op":"required","id":)" + std::to_string(id) + R"(,"modules":[]})";
        }

        // Step 1: Parse ALL forms first
        read::lex::processor l_prc{ source };
        read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

        native_vector<runtime::object_ref> all_forms;
        for(auto const &form : p_prc)
        {
          all_forms.push_back(form.expect_ok().unwrap().ptr);
        }

        if(all_forms.empty())
        {
          return error_response(id, "No expressions in namespace source", "compile");
        }

        // Step 2: Evaluate the ns form (first form) to register native aliases
        // This will JIT-load all transitive dependencies on macOS
        auto const ns_form = all_forms.front();
        std::cerr << "[compile-server] Evaluating ns form: " << runtime::to_string(ns_form) << std::endl;

        try
        {
          // Establish thread bindings for this worker thread
          runtime::context::binding_scope const scope{
            runtime::obj::persistent_hash_map::create_unique(
              std::make_pair(runtime::__rt_ctx->current_ns_var,
                             runtime::__rt_ctx->current_ns_var->deref()))
          };

          runtime::__rt_ctx->eval(ns_form);
          std::cerr << "[compile-server] ns form evaluated successfully" << std::endl;
        }
        catch(jtl::immutable_string const &e)
        {
          return error_response(id, util::format("ns eval failed: {}", std::string(e.data(), e.size())), "compile");
        }
        catch(runtime::object_ref const &e)
        {
          return error_response(id, util::format("ns eval failed: {}", runtime::to_string(e)), "compile");
        }
        catch(jtl::ref<error::base> const &e)
        {
          std::string msg = std::string(error::kind_str(e->kind)) + ": " + std::string(e->message.data(), e->message.size());
          return error_response(id, util::format("ns eval failed: {}", msg), "compile");
        }
        catch(std::exception const &e)
        {
          return error_response(id, util::format("ns eval failed: {}", e.what()), "compile");
        }
        catch(...)
        {
          auto const *ti = abi::__cxa_current_exception_type();
          if(ti)
          {
            int status = 0;
            char *demangled = abi::__cxa_demangle(ti->name(), nullptr, nullptr, &status);
            std::string type_name = (status == 0 && demangled) ? demangled : ti->name();
            if(demangled) free(demangled);
            return error_response(id, util::format("ns eval failed (type: {})", type_name), "compile");
          }
          return error_response(id, "ns eval failed: unknown error", "compile");
        }

        // Step 3: Get ALL modules that need to be compiled for iOS
        // We check ALL loaded modules (not just newly loaded) because dependencies
        // may already be loaded on macOS from the desktop app initialization.
        // We compile any module that hasn't been sent to iOS yet.
        native_vector<jtl::immutable_string> modules_to_compile;
        {
          auto locked = runtime::__rt_ctx->loaded_modules_in_order.rlock();
          for(auto const &mod : *locked)
          {
            // Skip if already sent to iOS
            std::string mod_str(mod.data(), mod.size());
            if(loaded_namespaces_.find(mod_str) != loaded_namespaces_.end())
            {
              continue;
            }
            modules_to_compile.push_back(mod);
          }
        }
        std::cout << "[compile-server] Modules to compile for iOS: " << modules_to_compile.size() << std::endl;

        // Step 4: Collect all modules to compile
        // Start with transitive dependencies (in load order), then add the main module last
        native_vector<compiled_module_info> compiled_modules;
        native_vector<std::string> compilation_errors;

        for(auto const &dep_module : modules_to_compile)
        {
          // Skip core modules (clojure.core, clojure.string, etc.)
          if(runtime::module::is_core_module(dep_module))
          {
            std::cout << "[compile-server] Skipping core module: " << dep_module << std::endl;
            continue;
          }

          std::string dep_module_str(dep_module.data(), dep_module.size());

          // Find and read the module source
          std::cout << "[compile-server] Compiling transitive dependency: " << dep_module << std::endl;

          auto const find_result = runtime::__rt_ctx->module_loader.find(dep_module, runtime::module::origin::latest);
          if(find_result.is_err())
          {
            std::cerr << "[compile-server] Warning: Could not find source for: " << dep_module << std::endl;
            continue;
          }

          auto const &entry = find_result.expect_ok();
          jtl::option<runtime::module::file_entry> source_entry;

          // Prefer .jank source, fall back to .cljc
          if(entry.sources.jank.is_some())
          {
            source_entry = entry.sources.jank;
          }
          else if(entry.sources.cljc.is_some())
          {
            source_entry = entry.sources.cljc;
          }

          if(source_entry.is_none())
          {
            std::cerr << "[compile-server] Warning: No jank/cljc source for: " << dep_module << std::endl;
            continue;
          }

          // Read the source file
          auto const file_view_result = runtime::module::loader::read_file(source_entry.unwrap().path);
          if(file_view_result.is_err())
          {
            std::cerr << "[compile-server] Warning: Could not read source for: " << dep_module << std::endl;
            continue;
          }

          auto const &file_view = file_view_result.expect_ok();
          std::string dep_source(file_view.data(), file_view.size());

          // Compile the dependency
          auto compiled = compile_namespace_source(id, dep_module_str, dep_source, compilation_errors);
          if(compiled.is_some())
          {
            loaded_namespaces_.insert(dep_module_str);
            compiled_modules.push_back(compiled.unwrap());
          }
        }

        // Step 5: Now compile the main module (same as before)
        // Create a NEW analyzer after ns evaluation
        analyze::processor an_prc;
        native_vector<analyze::expression_ref> exprs;

        // Extract target namespace name from ns form and look it up
        auto const ns_list = runtime::try_object<runtime::obj::persistent_list>(ns_form);
        if(runtime::sequence_length(ns_list) < 2)
        {
          return error_response(id, "Invalid ns form: expected (ns name ...)", "compile");
        }
        auto const ns_name_sym = runtime::try_object<runtime::obj::symbol>(ns_list->data.rest().first().unwrap());
        auto const target_ns = runtime::__rt_ctx->find_ns(ns_name_sym);
        if(target_ns.is_nil())
        {
          return error_response(id, util::format("Namespace {} not found after ns form evaluation",
                                                  ns_name_sym->to_string()), "compile");
        }

        // Analyze ALL forms with *ns* bound to the target namespace
        {
          runtime::context::binding_scope const analysis_scope{
            runtime::obj::persistent_hash_map::create_unique(
              std::make_pair(runtime::__rt_ctx->current_ns_var, target_ns))
          };

          for(auto const &parsed_form : all_forms)
          {
            auto expr = an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok();
            expr = analyze::pass::optimize(expr);
            exprs.push_back(expr);
          }
        }

        // Generate module name
        auto const module_name = jtl::immutable_string(ns_name) + "$loading__";

        // Wrap expressions in a function for codegen
        auto const fn_expr = evaluate::wrap_expressions(exprs, an_prc,
                                                        runtime::__rt_ctx->unique_munged_string("ns_load"));

        // Generate C++ code
        codegen::processor cg_prc{ fn_expr, module_name, codegen::compilation_target::module };
        auto const cpp_code_body = cg_prc.declaration_str();
        auto const munged_struct_name = std::string(runtime::munge(cg_prc.struct_name.name).data());
        auto const entry_symbol = "_" + munged_struct_name + "_0";

        auto const module_ns = runtime::module::module_to_native_ns(module_name);
        auto const qualified_struct = std::string(module_ns.data(), module_ns.size()) +
                                      "::" + munged_struct_name;

        std::string cpp_code;
        cpp_code += "#include <jank/prelude.hpp>\n";

        // Add native header includes from namespace
        auto const native_aliases = target_ns->native_aliases_snapshot();
        for(auto const &alias : native_aliases)
        {
          cpp_code += "#include <" + std::string(alias.header.data(), alias.header.size()) + ">\n";
          std::cout << "[compile-server] Adding native header: " << alias.header << std::endl;
        }
        cpp_code += "\n";

        cpp_code += std::string(cpp_code_body);
        cpp_code += "\n\n";
        cpp_code += "extern \"C\" ::jank::runtime::object_ref " + entry_symbol + "() {\n";
        cpp_code += "  return ::jank::runtime::make_box<" + qualified_struct + ">()->call();\n";
        cpp_code += "}\n";

        // Cross-compile to ARM64 object file
        auto const object_result = cross_compile(id, cpp_code);

        if(!object_result.success)
        {
          return error_response(id, object_result.error, "cross-compile");
        }

        loaded_namespaces_.insert(ns_name);

        // Add main module to compiled list
        compiled_modules.push_back(compiled_module_info{
          std::string(module_name.data(), module_name.size()),
          entry_symbol,
          base64_encode(object_result.object_data)
        });

        std::cout << "[compile-server] Namespace " << ns_name << " compiled successfully, object size: "
                  << object_result.object_data.size() << " bytes" << std::endl;
        std::cout << "[compile-server] Total modules compiled: " << compiled_modules.size() << std::endl;

        // Step 7: Build response with all compiled modules
        std::string response = R"({"op":"required","id":)" + std::to_string(id) + R"(,"modules":[)";
        bool first = true;
        for(auto const &mod : compiled_modules)
        {
          if(!first) response += ",";
          first = false;
          response += R"({"name":")" + escape_json(mod.name);
          response += R"(","symbol":")" + escape_json(mod.symbol);
          response += R"(","object":")" + mod.encoded_object + R"("})";
        }
        response += "]}";

        return response;
      }
      catch(jtl::ref<error::base> const &e)
      {
        std::string msg = std::string(error::kind_str(e->kind)) + ": "
                        + std::string(e->message.data(), e->message.size());
        return error_response(id, msg, "compile");
      }
      catch(std::exception const &e)
      {
        return error_response(id, e.what(), "compile");
      }
      catch(...)
      {
        return error_response(id, "Unknown compilation error", "compile");
      }
    }

    struct cross_compile_result
    {
      bool success{ false };
      std::vector<uint8_t> object_data;
      std::string error;
    };

    cross_compile_result cross_compile(int64_t id, std::string const &cpp_code)
    {
      // Write C++ to temp file
      auto const cpp_path = config_.temp_dir + "/compile_" + std::to_string(id) + ".cpp";
      auto const obj_path = config_.temp_dir + "/compile_" + std::to_string(id) + ".o";

      {
        std::ofstream cpp_file(cpp_path);
        if(!cpp_file)
        {
          return { false, {}, "Failed to create temp file: " + cpp_path };
        }
        cpp_file << cpp_code;
      }

      // Build clang command
      std::vector<std::string> args;
      args.push_back(config_.clang_path);
      args.push_back("-c");
      args.push_back("-target");
      args.push_back(config_.target_triple);
      args.push_back("-isysroot");
      args.push_back(config_.ios_sdk_path);
      args.push_back("-std=gnu++20");
      args.push_back("-fPIC");
      args.push_back("-O2");
      args.push_back("-w");  // Suppress warnings

      // Add PCH if available
      if(!config_.pch_path.empty() && std::filesystem::exists(config_.pch_path))
      {
        args.push_back("-include-pch");
        args.push_back(config_.pch_path);
      }

      // Add include paths
      for(auto const &inc : config_.include_paths)
      {
        args.push_back("-I" + inc);
      }

      // Add extra flags
      for(auto const &flag : config_.flags)
      {
        args.push_back(flag);
      }

      args.push_back("-o");
      args.push_back(obj_path);
      args.push_back(cpp_path);

      // Build command string
      std::string cmd;
      for(auto const &arg : args)
      {
        if(!cmd.empty()) cmd += " ";
        // Quote args with spaces
        if(arg.find(' ') != std::string::npos)
        {
          cmd += "\"" + arg + "\"";
        }
        else
        {
          cmd += arg;
        }
      }
      cmd += " 2>&1";  // Capture stderr

      std::cout << "[compile-server] Cross-compiling: " << cmd << std::endl;

      // Execute clang
      FILE *pipe = popen(cmd.c_str(), "r");
      if(!pipe)
      {
        return { false, {}, "Failed to execute clang" };
      }

      std::string output;
      char buf[256];
      while(fgets(buf, sizeof(buf), pipe))
      {
        output += buf;
      }

      int status = pclose(pipe);

      std::cout << "[compile-server] Clang exited with status: " << status << std::endl;
      if(!output.empty())
      {
        std::cout << "[compile-server] Clang output: " << output << std::endl;
      }

      // Keep source file for debugging if compilation fails
      if(status != 0)
      {
        std::cerr << "[compile-server] Compilation FAILED. Source kept at: " << cpp_path << std::endl;
        std::filesystem::remove(obj_path);
        return { false, {}, "Clang error: " + output };
      }

      // Clean up source file
      std::filesystem::remove(cpp_path);

      // Read object file
      std::ifstream obj_file(obj_path, std::ios::binary);
      if(!obj_file)
      {
        return { false, {}, "Failed to read object file" };
      }

      std::vector<uint8_t> obj_data(
        (std::istreambuf_iterator<char>(obj_file)),
        std::istreambuf_iterator<char>()
      );

      // Clean up object file
      std::filesystem::remove(obj_path);

      std::cout << "[compile-server] Compiled successfully, object size: "
                << obj_data.size() << " bytes" << std::endl;

      return { true, std::move(obj_data), "" };
    }

    std::string error_response(int64_t id, std::string const &error, std::string const &type)
    {
      std::cerr << "[compile-server] Error (id=" << id << ", type=" << type << "): " << error << std::endl;
      return R"({"op":"error","id":)" + std::to_string(id)
        + R"(,"error":")" + escape_json(error)
        + R"(","type":")" + type + R"("})";
    }

    // Simple JSON helpers
    static std::string get_json_string(std::string const &json, std::string const &key)
    {
      auto key_pos = json.find("\"" + key + "\"");
      if(key_pos == std::string::npos) return "";
      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos) return "";
      auto quote_start = json.find('"', colon_pos);
      if(quote_start == std::string::npos) return "";
      auto quote_end = quote_start + 1;
      while(quote_end < json.size())
      {
        if(json[quote_end] == '"' && (quote_end == 0 || json[quote_end - 1] != '\\'))
          break;
        quote_end++;
      }
      std::string result;
      for(size_t i = quote_start + 1; i < quote_end; i++)
      {
        if(json[i] == '\\' && i + 1 < quote_end)
        {
          switch(json[i + 1])
          {
            case 'n': result += '\n'; i++; break;
            case 'r': result += '\r'; i++; break;
            case 't': result += '\t'; i++; break;
            case '"': result += '"'; i++; break;
            case '\\': result += '\\'; i++; break;
            default: result += json[i];
          }
        }
        else
        {
          result += json[i];
        }
      }
      return result;
    }

    static int64_t get_json_int(std::string const &json, std::string const &key)
    {
      auto key_pos = json.find("\"" + key + "\"");
      if(key_pos == std::string::npos) return 0;
      auto colon_pos = json.find(':', key_pos);
      if(colon_pos == std::string::npos) return 0;
      auto num_start = colon_pos + 1;
      while(num_start < json.size() && (json[num_start] == ' ' || json[num_start] == '\t'))
        num_start++;
      try { return std::stoll(json.substr(num_start)); }
      catch(...) { return 0; }
    }

    static std::string escape_json(std::string const &s)
    {
      std::string result;
      result.reserve(s.size() + 16);
      for(char c : s)
      {
        switch(c)
        {
          case '"': result += "\\\""; break;
          case '\\': result += "\\\\"; break;
          case '\n': result += "\\n"; break;
          case '\r': result += "\\r"; break;
          case '\t': result += "\\t"; break;
          default: result += c;
        }
      }
      return result;
    }

    // Base64 encoding for object file
    static std::string base64_encode(std::vector<uint8_t> const &data)
    {
      static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

      std::string result;
      result.reserve(((data.size() + 2) / 3) * 4);

      for(size_t i = 0; i < data.size(); i += 3)
      {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if(i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if(i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < data.size()) ? table[n & 0x3F] : '=');
      }

      return result;
    }

    uint16_t port_;
    ios_compile_config config_;
    boost::asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::atomic<bool> running_;
    pthread_t server_thread_;
    std::unordered_set<std::string> loaded_namespaces_;  // Track loaded namespaces

    // Helper struct for compiled module info
    struct compiled_module_info
    {
      std::string name;
      std::string symbol;
      std::string encoded_object;
    };

    // Compile a single namespace from source (helper for require_ns)
    // Returns empty option on failure (error already logged)
    jtl::option<compiled_module_info> compile_namespace_source(
      int64_t id,
      std::string const &ns_name,
      std::string const &source,
      native_vector<std::string> &errors)
    {
      try
      {
        // Parse ALL forms first
        read::lex::processor l_prc{ source };
        read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };

        native_vector<runtime::object_ref> all_forms;
        for(auto const &form : p_prc)
        {
          all_forms.push_back(form.expect_ok().unwrap().ptr);
        }

        if(all_forms.empty())
        {
          errors.push_back("No expressions in namespace source for: " + ns_name);
          return jtl::none;
        }

        // Get the ns form
        auto const ns_form = all_forms.front();

        // Extract target namespace name from ns form and look it up
        auto const ns_list = runtime::try_object<runtime::obj::persistent_list>(ns_form);
        if(runtime::sequence_length(ns_list) < 2)
        {
          errors.push_back("Invalid ns form for: " + ns_name);
          return jtl::none;
        }
        auto const ns_name_sym = runtime::try_object<runtime::obj::symbol>(ns_list->data.rest().first().unwrap());
        auto const target_ns = runtime::__rt_ctx->find_ns(ns_name_sym);
        if(target_ns.is_nil())
        {
          errors.push_back("Namespace not found after ns form evaluation: " + ns_name);
          return jtl::none;
        }

        // Analyze ALL forms with *ns* bound to the target namespace
        analyze::processor an_prc;
        native_vector<analyze::expression_ref> exprs;

        {
          runtime::context::binding_scope const analysis_scope{
            runtime::obj::persistent_hash_map::create_unique(
              std::make_pair(runtime::__rt_ctx->current_ns_var, target_ns))
          };

          for(auto const &parsed_form : all_forms)
          {
            auto expr = an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok();
            expr = analyze::pass::optimize(expr);
            exprs.push_back(expr);
          }
        }

        // Generate module name
        auto const module_name = jtl::immutable_string(ns_name) + "$loading__";

        // Wrap expressions in a function for codegen
        auto const fn_expr = evaluate::wrap_expressions(exprs, an_prc,
                                                        runtime::__rt_ctx->unique_munged_string("ns_load"));

        // Generate C++ code
        codegen::processor cg_prc{ fn_expr, module_name, codegen::compilation_target::module };
        auto const cpp_code_body = cg_prc.declaration_str();
        auto const munged_struct_name = std::string(runtime::munge(cg_prc.struct_name.name).data());
        auto const entry_symbol = "_" + munged_struct_name + "_0";

        auto const module_ns = runtime::module::module_to_native_ns(module_name);
        auto const qualified_struct = std::string(module_ns.data(), module_ns.size()) +
                                      "::" + munged_struct_name;

        std::string cpp_code;
        cpp_code += "#include <jank/prelude.hpp>\n";

        // Add native header includes from namespace
        auto const native_aliases = target_ns->native_aliases_snapshot();
        for(auto const &alias : native_aliases)
        {
          cpp_code += "#include <" + std::string(alias.header.data(), alias.header.size()) + ">\n";
        }
        cpp_code += "\n";

        cpp_code += std::string(cpp_code_body);
        cpp_code += "\n\n";
        cpp_code += "extern \"C\" ::jank::runtime::object_ref " + entry_symbol + "() {\n";
        cpp_code += "  return ::jank::runtime::make_box<" + qualified_struct + ">()->call();\n";
        cpp_code += "}\n";

        // Cross-compile to ARM64 object file
        auto const object_result = cross_compile(id, cpp_code);

        if(!object_result.success)
        {
          errors.push_back("Cross-compile failed for " + ns_name + ": " + object_result.error);
          return jtl::none;
        }

        std::cout << "[compile-server] Compiled dependency: " << ns_name
                  << " (" << object_result.object_data.size() << " bytes)" << std::endl;

        return compiled_module_info{
          std::string(module_name.data(), module_name.size()),
          entry_symbol,
          base64_encode(object_result.object_data)
        };
      }
      catch(std::exception const &e)
      {
        errors.push_back("Exception compiling " + ns_name + ": " + e.what());
        return jtl::none;
      }
      catch(...)
      {
        errors.push_back("Unknown exception compiling " + ns_name);
        return jtl::none;
      }
    }
  };

  // Helper to create default config for iOS Simulator
  inline ios_compile_config make_ios_simulator_config(
    std::string const &jank_resource_dir,
    std::string const &clang_path = "")
  {
    ios_compile_config config;

    // Find clang
    if(clang_path.empty())
    {
      // Try jank's bundled clang first
      auto bundled = jank_resource_dir + "/bin/clang++";
      if(std::filesystem::exists(bundled))
      {
        config.clang_path = bundled;
      }
      else
      {
        config.clang_path = "clang++";  // System clang
      }
    }
    else
    {
      config.clang_path = clang_path;
    }

    // iOS Simulator SDK
    config.ios_sdk_path = "/Applications/Xcode.app/Contents/Developer/Platforms/"
                          "iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk";

    // Target triple for simulator
    config.target_triple = "arm64-apple-ios17.0-simulator";

    // PCH for iOS (if it exists)
    config.pch_path = jank_resource_dir + "/incremental.pch";

    // Include paths - handle dev builds where jank_resource_dir/include doesn't exist
    auto const configured_include = jank_resource_dir + "/include";
    if(std::filesystem::exists(configured_include))
    {
      config.include_paths.push_back(configured_include);
    }
    else
    {
      // Dev build: try to find compiler+runtime/include/cpp relative to process dir
      // Process is at compiler+runtime/build/compile-server
      std::filesystem::path const process_path{ util::process_dir().c_str() };
      auto const compiler_runtime_dir = process_path.parent_path();
      auto const dev_build_include = compiler_runtime_dir / "include" / "cpp";
      if(std::filesystem::exists(dev_build_include))
      {
        config.include_paths.push_back(dev_build_include.string());
        std::cout << "[compile-server] Using dev build include path: " << dev_build_include << std::endl;

        // Add third-party include paths for dev build
        auto const third_party = compiler_runtime_dir / "third-party";
        config.include_paths.push_back((third_party / "bdwgc" / "include").string());
        config.include_paths.push_back((third_party / "immer").string());
        config.include_paths.push_back((third_party / "bpptree" / "include").string());
        config.include_paths.push_back((third_party / "folly").string());
        config.include_paths.push_back((third_party / "boost-preprocessor" / "include").string());
        config.include_paths.push_back((third_party / "boost-multiprecision" / "include").string());
        config.include_paths.push_back((third_party / "stduuid" / "include").string());
        config.include_paths.push_back((third_party / "cppinterop" / "include").string());
        config.include_paths.push_back((third_party / "cppinterop" / "lib").string());
        std::cout << "[compile-server] Added third-party include paths" << std::endl;
      }
      else
      {
        // Fall back to configured path anyway
        config.include_paths.push_back(configured_include);
        std::cerr << "[compile-server] Warning: Include path does not exist: " << configured_include << std::endl;
      }
    }

    // Add user-specified include paths from CLI (e.g., -I/path/to/project)
    // These are needed to find native headers like vulkan/sdf_engine.hpp
    for(auto const &inc : util::cli::opts.include_dirs)
    {
      config.include_paths.push_back(inc);
      std::cout << "[compile-server] Added user include path: " << inc << std::endl;
    }

    // Temp directory
    config.temp_dir = "/tmp/jank-compile-server";

    return config;
  }

} // namespace jank::compile_server
