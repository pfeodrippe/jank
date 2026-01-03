#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_stdin(message const &msg)
  {
    auto chunk(msg.get("stdin"));
    if(chunk.empty())
    {
      return handle_unsupported(msg, "missing-stdin");
    }

    auto &session(ensure_session(msg.session()));
    session.stdin_buffer += chunk;

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("stdin", chunk);
    payload.emplace("unread", session.stdin_buffer);
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
