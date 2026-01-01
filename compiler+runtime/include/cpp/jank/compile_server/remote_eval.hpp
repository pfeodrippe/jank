#pragma once

// iOS Remote Eval
//
// Evaluates jank code by delegating compilation to macOS, then loading
// the cross-compiled ARM64 object file via ORC JIT on iOS.
//
// This solves the symbol duplication problem where CppInterOp on iOS
// creates tentative definitions for extern globals (like GImGui) when
// parsing inline C++ functions. By doing compilation on macOS, the
// generated object files have proper relocations that get resolved at
// load time using the actual symbol addresses on iOS.
//
// Flow:
//   1. iOS sends jank code to macOS compile server
//   2. macOS compiles to C++, cross-compiles to ARM64 object file
//   3. macOS returns object file (base64 encoded) + entry symbol
//   4. iOS loads object via ORC JIT's addObjectFile()
//   5. iOS calls entry symbol to execute the code
//   6. Entry symbol returns the result as a jank object

#include <string>
#include <memory>
#include <iostream>

#include <jank/compile_server/client.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/error.hpp>

namespace jank::compile_server
{
  // Result type for remote eval
  struct remote_eval_result
  {
    bool success{ false };
    runtime::object_ref value{ runtime::jank_nil };
    std::string error;
    std::string error_type; // "connection", "compile", "load", "runtime"
  };

  class remote_eval
  {
  public:
    remote_eval(std::string const &host = "127.0.0.1", uint16_t port = default_compile_port)
      : client_(std::make_unique<client>(host, port))
      , current_ns_("user")
    {
    }

    // Set compile server address
    void set_server(std::string const &host, uint16_t port = default_compile_port)
    {
      client_->disconnect();
      client_ = std::make_unique<client>(host, port);
    }

    // Connect to compile server
    bool connect()
    {
      return client_->connect();
    }

    // Check connection
    bool is_connected() const
    {
      return client_->is_connected();
    }

    // Ping server
    bool ping()
    {
      return client_->ping();
    }

    // Evaluate jank code remotely
    remote_eval_result eval(std::string const &code, std::string const &ns = "")
    {
      remote_eval_result result;

      // Use provided namespace or current namespace
      std::string const effective_ns = ns.empty() ? current_ns_ : ns;

      // Request compilation from macOS
      auto compile_result = client_->compile(code, effective_ns, "");

      if(!compile_result.success)
      {
        result.success = false;
        result.error = compile_result.error;
        result.error_type = compile_result.error_type;
        return result;
      }

      // Load the object file into ORC JIT
      if(!load_object(compile_result.object_data, compile_result.entry_symbol))
      {
        result.success = false;
        result.error = "Failed to load compiled object into JIT";
        result.error_type = "load";
        return result;
      }

      // Find and call the entry symbol
      try
      {
        auto &jit_prc = runtime::__rt_ctx->jit_prc;
        auto const symbol_result = jit_prc.find_symbol(compile_result.entry_symbol.c_str());

        if(symbol_result.is_err())
        {
          result.success = false;
          result.error = "Entry symbol not found: " + compile_result.entry_symbol;
          result.error_type = "load";
          return result;
        }

        // The entry symbol is a function pointer: object_ref (*)()
        using entry_fn_t = runtime::object_ref (*)();
        auto entry_fn = reinterpret_cast<entry_fn_t>(symbol_result.expect_ok());

        // Call the entry function
        result.value = entry_fn();
        result.success = true;
      }
      catch(runtime::object_ref const &e)
      {
        result.success = false;
        auto msg = runtime::to_code_string(e);
        result.error = std::string(msg.data(), msg.size());
        result.error_type = "runtime";
      }
      catch(jtl::ref<error::base> const &e)
      {
        result.success = false;
        result.error = std::string(e->message.data(), e->message.size());
        result.error_type = "runtime";
      }
      catch(std::exception const &e)
      {
        result.success = false;
        result.error = e.what();
        result.error_type = "runtime";
      }
      catch(...)
      {
        result.success = false;
        result.error = "Unknown exception during execution";
        result.error_type = "runtime";
      }

      return result;
    }

    // Set current namespace
    void set_ns(std::string const &ns)
    {
      current_ns_ = ns;
    }

    std::string const &current_ns() const
    {
      return current_ns_;
    }

  private:
    bool load_object(std::vector<uint8_t> const &object_data, std::string const &name)
    {
      if(object_data.empty())
      {
        std::cerr << "[remote-eval] Empty object data" << std::endl;
        return false;
      }

      try
      {
        auto &jit_prc = runtime::__rt_ctx->jit_prc;

        // Use the processor's load_object method
        bool result = jit_prc.load_object(reinterpret_cast<char const *>(object_data.data()),
                                          object_data.size(),
                                          name);

        if(result)
        {
          std::cout << "[remote-eval] Loaded object (" << object_data.size() << " bytes) into JIT"
                    << std::endl;
        }

        return result;
      }
      catch(std::exception const &e)
      {
        std::cerr << "[remote-eval] Failed to load object: " << e.what() << std::endl;
        return false;
      }
    }

    std::unique_ptr<client> client_;
    std::string current_ns_;
  };

  // Global remote eval instance for iOS
  inline std::unique_ptr<remote_eval> &get_remote_eval_ptr()
  {
    static std::unique_ptr<remote_eval> instance;
    return instance;
  }

  // Initialize remote eval with compile server address
  inline void
  init_remote_eval(std::string const &host = "127.0.0.1", uint16_t port = default_compile_port)
  {
    auto &ptr = get_remote_eval_ptr();
    ptr = std::make_unique<remote_eval>(host, port);
  }

  // Get remote eval instance
  inline remote_eval *get_remote_eval()
  {
    return get_remote_eval_ptr().get();
  }

} // namespace jank::compile_server
