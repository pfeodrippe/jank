#pragma once

// nREPL Operations for Remote Compilation
//
// These operations allow runtime configuration of remote JIT compilation
// from an nREPL client connected to iOS.
//
// Operations:
//   {"op":"remote-compile-connect","host":"192.168.1.100","port":5570}
//   {"op":"remote-compile-disconnect"}
//   {"op":"remote-compile-status"}

#ifdef JANK_IOS_JIT
#include <jank/compile_server/remote_compile.hpp>
#endif

namespace jank::nrepl_server::asio
{
#ifndef __EMSCRIPTEN__

  inline std::vector<bencode::value::dict>
  engine::handle_remote_compile_connect([[maybe_unused]] message const &msg)
  {
#ifdef JANK_IOS_JIT
    auto const host(msg.get("host"));
    auto const port_opt(msg.get_integer("port"));
    uint16_t port = port_opt.value_or(compile_server::default_compile_port);

    if(host.empty())
    {
      bencode::value::dict response;
      response.emplace("status", bencode::list_of_strings({ "error", "done" }));
      response.emplace("err", "Missing 'host' parameter");
      if(!msg.id().empty())
      {
        response.emplace("id", msg.id());
      }
      if(!msg.session().empty())
      {
        response.emplace("session", msg.session());
      }
      return { std::move(response) };
    }

    compile_server::configure_remote_compile(host, port);
    bool success = compile_server::connect_remote_compile();

    bencode::value::dict response;
    if(success)
    {
      response.emplace("status", bencode::list_of_strings({ "done" }));
      response.emplace("remote-compile-host", host);
      response.emplace("remote-compile-port", static_cast<int64_t>(port));
      response.emplace("remote-compile-connected", "true");
    }
    else
    {
      response.emplace("status", bencode::list_of_strings({ "error", "done" }));
      response.emplace("err", "Failed to connect to compile server at " + host + ":" + std::to_string(port));
    }

    if(!msg.id().empty())
    {
      response.emplace("id", msg.id());
    }
    if(!msg.session().empty())
    {
      response.emplace("session", msg.session());
    }

    return { std::move(response) };
#else
    bencode::value::dict response;
    response.emplace("status", bencode::list_of_strings({ "error", "done" }));
    response.emplace("err", "Remote compilation is only available on iOS");
    if(!msg.id().empty())
    {
      response.emplace("id", msg.id());
    }
    if(!msg.session().empty())
    {
      response.emplace("session", msg.session());
    }
    return { std::move(response) };
#endif
  }

  inline std::vector<bencode::value::dict>
  engine::handle_remote_compile_disconnect([[maybe_unused]] message const &msg)
  {
#ifdef JANK_IOS_JIT
    compile_server::disconnect_remote_compile();
#endif

    bencode::value::dict response;
    response.emplace("status", bencode::list_of_strings({ "done" }));
    response.emplace("remote-compile-connected", "false");

    if(!msg.id().empty())
    {
      response.emplace("id", msg.id());
    }
    if(!msg.session().empty())
    {
      response.emplace("session", msg.session());
    }

    return { std::move(response) };
  }

  inline std::vector<bencode::value::dict>
  engine::handle_remote_compile_status([[maybe_unused]] message const &msg)
  {
    bencode::value::dict response;
    response.emplace("status", bencode::list_of_strings({ "done" }));

#ifdef JANK_IOS_JIT
    if(compile_server::is_remote_compile_enabled())
    {
      std::lock_guard<std::mutex> lock(compile_server::remote_config_mutex);
      response.emplace("remote-compile-connected", "true");
      response.emplace("remote-compile-host", compile_server::remote_config.host);
      response.emplace("remote-compile-port",
                       static_cast<int64_t>(compile_server::remote_config.port));
    }
    else
    {
      response.emplace("remote-compile-connected", "false");
    }
#else
    response.emplace("remote-compile-available", "false");
    response.emplace("remote-compile-connected", "false");
#endif

    if(!msg.id().empty())
    {
      response.emplace("id", msg.id());
    }
    if(!msg.session().empty())
    {
      response.emplace("session", msg.session());
    }

    return { std::move(response) };
  }

#endif
} // namespace jank::nrepl_server::asio
