#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_swap_middleware(message const &msg)
  {
    auto maybe_items(parse_string_list(msg.data, "middleware"));
    if(!maybe_items.has_value())
    {
      return handle_unsupported(msg, "missing-middleware");
    }

    std::set<std::string> existing{ middleware_stack_.begin(), middleware_stack_.end() };
    std::set<std::string> incoming{ maybe_items->begin(), maybe_items->end() };
    if(existing != incoming)
    {
      return handle_unsupported(msg, "middleware-mismatch");
    }

    middleware_stack_ = maybe_items.value();
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
