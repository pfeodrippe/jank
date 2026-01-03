#pragma once

// iOS Remote Eval Operations for nREPL
//
// This implements Piggieback-style remote eval, where the nREPL server
// forwards eval requests to an iOS device running the minimal eval server.
//
// Usage from nREPL client:
//   (nrepl/ios-connect "192.168.1.100" 5558)  ; Connect to iOS device
//   (+ 1 2)                                    ; Eval happens on iOS!
//   (nrepl/ios-disconnect)                     ; Disconnect

// The utility functions (is_remote_eval_active, eval_on_ios, etc.) are in
// ios_remote_eval.hpp which is included earlier in engine.hpp.

namespace jank::nrepl_server::asio
{
#ifndef __EMSCRIPTEN__
  // nREPL operation handlers for iOS eval

  inline std::vector<bencode::value::dict>
  engine::handle_ios_connect([[maybe_unused]] message const &msg)
  {
    auto const host(msg.get("host"));
    auto const port_opt(msg.get_integer("port"));
    uint16_t port = port_opt.value_or(5558);

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

    bool success = connect_ios_eval(host, port);

    bencode::value::dict response;
    if(success)
    {
      response.emplace("status", bencode::list_of_strings({ "done" }));
      response.emplace("ios-host", host);
      response.emplace("ios-port", static_cast<int64_t>(port));
      response.emplace("ios-connected", "true");
    }
    else
    {
      response.emplace("status", bencode::list_of_strings({ "error", "done" }));
      response.emplace("err",
                       "Failed to connect to iOS device at " + host + ":" + std::to_string(port));
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
  }

  inline std::vector<bencode::value::dict>
  engine::handle_ios_disconnect([[maybe_unused]] message const &msg)
  {
    disconnect_ios_eval();

    bencode::value::dict response;
    response.emplace("status", bencode::list_of_strings({ "done" }));
    response.emplace("ios-connected", "false");

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
  engine::handle_ios_status([[maybe_unused]] message const &msg)
  {
    bencode::value::dict response;
    response.emplace("status", bencode::list_of_strings({ "done" }));

    if(is_remote_eval_active())
    {
      response.emplace("ios-connected", "true");
      response.emplace("ios-host", remote_target->host);
      response.emplace("ios-port", static_cast<int64_t>(remote_target->port));
    }
    else
    {
      response.emplace("ios-connected", "false");
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
  }
#endif

} // namespace jank::nrepl_server::asio
