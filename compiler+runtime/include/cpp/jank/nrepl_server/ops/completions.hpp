#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_completions(message const &msg)
  {
    auto const prefix(msg.get("prefix"));
    auto &session(ensure_session(msg.session()));
    auto const requested_ns(msg.get("ns"));
    auto const context(msg.get("context"));

    bencode::value::list completions;

    /* Check if we're in a require context - if so, provide namespace completions */
    if(is_require_context(context))
    {
      auto const ns_candidates(make_namespace_candidates(prefix));
      completions.reserve(ns_candidates.size());
      for(auto const &candidate : ns_candidates)
      {
        bencode::value::dict entry;
        entry.emplace("candidate", candidate.display_name);
        entry.emplace("type", "namespace");
        completions.emplace_back(std::move(entry));
      }
    }
    else
    {
      auto const query(
        prepare_completion_query(session, prefix, requested_ns, msg.get("symbol")));
      auto const candidates(make_completion_candidates(query));

      completions.reserve(candidates.size());
      for(auto const &candidate : candidates)
      {
        std::string type_label{ "var" };
        if(query.target_ns->name->name == "cpp")
        {
          if(auto info = describe_cpp_entity(query.target_ns, candidate.symbol_name))
          {
            type_label = completion_type_for(info.value());
          }
          else
          {
            type_label = "function";
          }
        }
        else if(query.native_alias.has_value())
        {
          type_label = "function";
        }

        bencode::value::dict entry;
        entry.emplace("candidate", candidate.display_name);
        entry.emplace("type", type_label);
        completions.emplace_back(std::move(entry));
      }
    }

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("completions", bencode::value{ std::move(completions) });
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
