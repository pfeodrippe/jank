#pragma once

// iOS Remote Eval Utilities
//
// This provides the utility functions for connecting to and evaluating code on
// a remote iOS device. These functions must be available before eval.hpp is included.
//
// The engine handler methods are defined in ops/ios_eval.hpp.

#include <string>
#include <optional>
#include <memory>
#include <iostream>

#ifndef __EMSCRIPTEN__
  #include <jank/ios/eval_client.hpp>
#endif

namespace jank::nrepl_server::asio
{
#ifndef __EMSCRIPTEN__
  // Remote eval target configuration
  // Uses unique_ptr because eval_client contains non-copyable socket
  struct remote_eval_target
  {
    std::string host;
    uint16_t port{ 5558 };
    bool connected{ false };
    std::unique_ptr<ios::eval_client> client;

    remote_eval_target()
      : client{ std::make_unique<ios::eval_client>() }
    {
    }
  };

  // Global remote eval target (one per nREPL server instance)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline std::unique_ptr<remote_eval_target> remote_target;

  // Check if remote eval is active
  inline bool is_remote_eval_active()
  {
    return remote_target && remote_target->connected;
  }

  // Connect to iOS eval server
  inline bool connect_ios_eval(std::string const &host, uint16_t port = 5558)
  {
    if(remote_target && remote_target->connected)
    {
      // Disconnect existing
      remote_target->client->disconnect();
    }

    remote_target = std::make_unique<remote_eval_target>();
    remote_target->host = host;
    remote_target->port = port;

    if(remote_target->client->connect(host, port))
    {
      remote_target->connected = true;
      std::cout << "[nrepl] Connected to iOS eval server at " << host << ":" << port << std::endl;
      std::cout << "[nrepl] All eval requests will be forwarded to iOS device." << std::endl;
      return true;
    }
    else
    {
      std::cerr << "[nrepl] Failed to connect to iOS: " << remote_target->client->last_error()
                << std::endl;
      remote_target.reset();
      return false;
    }
  }

  // Disconnect from iOS eval server
  inline void disconnect_ios_eval()
  {
    if(remote_target)
    {
      if(remote_target->connected)
      {
        remote_target->client->disconnect();
        std::cout << "[nrepl] Disconnected from iOS eval server." << std::endl;
      }
      remote_target.reset();
    }
  }

  // Evaluate code on remote iOS device
  // Returns pair<success, result_or_error>
  inline std::pair<bool, std::string>
  eval_on_ios(std::string const &code, std::string const &ns = "user")
  {
    if(!is_remote_eval_active())
    {
      return { false, "Not connected to iOS device" };
    }

    // Prepend namespace switch if needed (Piggieback-style)
    // This keeps the iOS eval server simple - it just evals what it receives
    std::string full_code;
    if(!ns.empty() && ns != "user")
    {
      full_code = "(in-ns '" + ns + ") " + code;
    }
    else
    {
      full_code = code;
    }

    auto result = remote_target->client->eval(full_code, ns);

    if(result.success)
    {
      return { true, result.value };
    }
    else
    {
      // Check if connection was lost
      if(result.error_type == "connection")
      {
        remote_target->connected = false;
        std::cerr << "[nrepl] Lost connection to iOS device." << std::endl;
      }
      // Include error type in message for better debugging
      std::string error_msg = result.error;
      if(error_msg.empty())
      {
        error_msg = "Unknown error";
      }
      if(!result.error_type.empty())
      {
        error_msg = "[" + result.error_type + "] " + error_msg;
      }
      return { false, error_msg };
    }
  }

#else
  // WASM stubs
  inline bool is_remote_eval_active()
  {
    return false;
  }

  inline bool connect_ios_eval(std::string const &, uint16_t = 5558)
  {
    return false;
  }

  inline void disconnect_ios_eval()
  {
  }

  inline std::pair<bool, std::string> eval_on_ios(std::string const &, std::string const & = "user")
  {
    return { false, "Remote eval not supported on WASM" };
  }
#endif

} // namespace jank::nrepl_server::asio
