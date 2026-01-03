#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_forward_system_output(message const &msg)
  {
    auto &session(ensure_session(msg.session()));
    session.forward_system_output = true;

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
