#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_completions(message const &msg)
  {
    auto const prefix(msg.get("prefix"));
    auto &session(ensure_session(msg.session()));
    auto const requested_ns(msg.get("ns"));
    auto const query(prepare_completion_query(session, prefix, requested_ns, msg.get("symbol")));
    auto const candidates(make_completion_candidates(query));

    bencode::value::list completions;
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

      bencode::value::dict entry;
      entry.emplace("candidate", candidate.display_name);
      entry.emplace("type", type_label);
      completions.emplace_back(std::move(entry));
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
