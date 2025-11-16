#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_interrupt(message const &msg)
  {
    auto const target_id(msg.get("interrupt-id"));
    if(target_id.empty())
    {
      return handle_unsupported(msg, "missing-interrupt-id");
    }

    auto &session(ensure_session(msg.session()));
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("interrupt-id", target_id);

    if(session.active_request_id == target_id)
    {
      payload.emplace("status", bencode::list_of_strings({ "interrupt-unsent", "done" }));
    }
    else
    {
      payload.emplace("status", bencode::list_of_strings({ "session-idle", "done" }));
    }
    return { std::move(payload) };
  }
}
