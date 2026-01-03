#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_ls_sessions(message const &msg)
  {
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for(auto const &entry : sessions_)
    {
      ids.push_back(entry.first);
    }
    std::ranges::sort(ids);

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("sessions", bencode::list_of_strings(ids));
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
