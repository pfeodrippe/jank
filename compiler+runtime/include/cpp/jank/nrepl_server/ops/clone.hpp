#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_clone(message const &msg)
  {
    auto const parent(msg.session());
    auto const &parent_session(ensure_session(parent));

    auto const new_id(next_session_id());
    auto &child(sessions_[new_id]);
    child.id = new_id;
    child.current_ns = parent_session.current_ns;
    child.forward_system_output = parent_session.forward_system_output;

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", new_id);
    payload.emplace("new-session", new_id);
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
