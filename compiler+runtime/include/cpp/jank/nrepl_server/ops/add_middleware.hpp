#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_add_middleware(message const &msg)
  {
    auto maybe_items(parse_string_list(msg.data, "middleware"));
    if(!maybe_items.has_value())
    {
      return handle_unsupported(msg, "missing-middleware");
    }

    for(auto const &entry : maybe_items.value())
    {
      auto const already(std::find(middleware_stack_.begin(), middleware_stack_.end(), entry));
      if(already == middleware_stack_.end())
      {
        middleware_stack_.push_back(entry);
      }
    }

    auto &session(ensure_session(msg.session()));
    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("middleware", bencode::list_of_strings(middleware_stack_));
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
