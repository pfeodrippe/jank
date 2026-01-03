#pragma once

// Remote Compilation Configuration for iOS
//
// This module provides configuration for remote JIT compilation where:
// - iOS sends jank source code to macOS compile-server
// - macOS cross-compiles to ARM64 object files
// - iOS loads the object files via ORC JIT and executes
//
// This enables full JIT compilation on iOS without requiring CppInterOp
// or the heavy LLVM/Clang infrastructure on the device.
//
// Usage:
//   // At app startup, configure remote compilation
//   jank::compile_server::configure_remote_compile("192.168.1.100", 5570);
//   jank::compile_server::connect_remote_compile();
//
//   // Later, eval_string will automatically use remote compilation
//   __rt_ctx->eval_string("(defn foo [] 42)");

#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>

#include <jank/compile_server/client.hpp>

namespace jank::compile_server
{
  struct remote_compile_config
  {
    std::string host{ "127.0.0.1" };
    uint16_t port{ default_compile_port };
    std::atomic<bool> enabled{ false };
  };

  // Global configuration for remote compilation
  // Thread-safe access via mutex
  extern remote_compile_config remote_config;
  extern std::mutex remote_config_mutex;
  extern std::unique_ptr<client> remote_client;

  // Configure remote compilation (must be called before connect)
  inline void configure_remote_compile(std::string const &host, uint16_t port)
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    remote_config.host = host;
    remote_config.port = port;
  }

  // Connect to the compile server
  inline bool connect_remote_compile()
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    if(remote_client && remote_client->is_connected())
    {
      return true;
    }
    remote_client = std::make_unique<client>(remote_config.host, remote_config.port);
    if(remote_client->connect())
    {
      remote_config.enabled.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  // Disconnect from the compile server
  inline void disconnect_remote_compile()
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    remote_config.enabled.store(false, std::memory_order_release);
    if(remote_client)
    {
      remote_client->disconnect();
      remote_client.reset();
    }
  }

  // Check if remote compilation is enabled and connected
  inline bool is_remote_compile_enabled()
  {
    return remote_config.enabled.load(std::memory_order_acquire);
  }

  // Get the client for remote compilation (assumes lock is held or single-threaded context)
  inline client *get_remote_client()
  {
    return remote_client.get();
  }

  // Compile code remotely and return the result
  // Returns the object data and entry symbol, or error info
  inline compile_response remote_compile(std::string const &code, std::string const &ns = "user")
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    if(!remote_client)
    {
      compile_response resp;
      resp.success = false;
      resp.error = "Remote compile not connected";
      resp.error_type = "connection";
      return resp;
    }
    return remote_client->compile(code, ns);
  }

  // Require (load) a namespace remotely
  // Sends namespace source to compile server, gets back compiled modules
  inline require_response remote_require(std::string const &ns,
                                         std::string const &source,
                                         std::string const &source_path = "")
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    if(!remote_client)
    {
      require_response resp;
      resp.success = false;
      resp.error = "Remote compile not connected";
      resp.error_type = "connection";
      return resp;
    }
    return remote_client->require_ns(ns, source, source_path);
  }

  // Generate native C++ source remotely (for jank.compiler-native/native-source)
  // Sends form to compile server which has C++ headers loaded
  inline native_source_response
  remote_native_source(std::string const &code, std::string const &ns = "user")
  {
    std::lock_guard<std::mutex> lock(remote_config_mutex);
    if(!remote_client)
    {
      native_source_response resp;
      resp.success = false;
      resp.error = "Remote compile not connected";
      return resp;
    }
    return remote_client->native_source(code, ns);
  }

} // namespace jank::compile_server
