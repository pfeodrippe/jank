#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_close(message const &msg)
  {
    auto const session_id(msg.session());
    auto const erased(sessions_.erase(session_id));
    if(erased == 0)
    {
      return handle_unsupported(msg, "unknown-session");
    }

    if(!default_session_id_.empty() && default_session_id_ == session_id)
    {
      default_session_id_.clear();
    }

    return { make_done_response(session_id, msg.id(), { "done" }) };
  }
}
