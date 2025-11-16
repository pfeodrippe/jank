#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_caught(message const &msg)
  {
    auto &session(ensure_session(msg.session()));
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);

    if(session.last_exception_message.has_value())
    {
      payload.emplace("err", session.last_exception_message.value());
      if(session.last_exception_type.has_value())
      {
        payload.emplace("exception-type", session.last_exception_type.value());
      }
      payload.emplace("status", bencode::list_of_strings({ "done" }));
    }
    else
    {
      payload.emplace("status", bencode::list_of_strings({ "done", "no-error" }));
    }

    return { std::move(payload) };
  }
}
