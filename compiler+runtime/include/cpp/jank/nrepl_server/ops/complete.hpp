#pragma once

namespace jank::nrepl_server::asio
{
  inline std::vector<bencode::value::dict> engine::handle_complete(message const &msg)
  {
    auto prefix(msg.get("prefix"));
    auto const symbol_input(msg.get("symbol"));
    if(prefix.empty())
    {
      prefix = symbol_input;
    }
    if(prefix.empty())
    {
      return handle_unsupported(msg, "missing-prefix");
    }

    auto &session(ensure_session(msg.session()));
    auto const query(prepare_completion_query(session, prefix, msg.get("ns"), msg.get("symbol")));
    auto const candidates(make_completion_candidates(query));

    bool include_doc{ true };
    bool include_arglists{ true };
    bool include_ns_info{ true };
    if(auto const extra = parse_string_list(msg.data, "extra-metadata"))
    {
      std::unordered_set<std::string> normalized;
      normalized.reserve(extra->size());
      for(auto const &entry : extra.value())
      {
        auto const normalized_entry(normalize_metadata_key(entry));
        normalized.insert(normalized_entry);
      }
      include_doc = normalized.contains("doc");
      include_arglists = normalized.contains("arglists");
      include_ns_info = normalized.contains("ns");
    }

    bencode::value::list completion_payloads;
    completion_payloads.reserve(candidates.size());
    for(auto const &candidate : candidates)
    {
      bool const prefer_native_header = query.native_alias.has_value()
        && query.qualifier.has_value()
        && native_header_index_.contains(query.native_alias.value(), candidate.symbol_name);

      auto const symbol(make_box<obj::symbol>(make_immutable_string(candidate.symbol_name)));
      auto const var(query.target_ns->find_var(symbol));
      std::optional<var_documentation> var_info;
      auto const describe_native_candidate = [&]() {
        return describe_native_header_function(query.qualifier.value(),
                                               query.native_alias.value(),
                                               candidate.symbol_name);
      };

      // Check if this symbol is a native refer (unqualified symbol referring to a native header function)
      if(!var_info.has_value() && !query.qualifier.has_value())
      {
        auto const native_refer = query.target_ns->find_native_refer(symbol);
        if(native_refer.is_some())
        {
          auto const &refer_info = native_refer.unwrap();
          // Get the alias name to look up in the native header index
          auto const alias_name = to_std_string(refer_info.alias->name);
          var_info = describe_native_header_function(
            alias_name,
            query.target_ns->find_native_alias(refer_info.alias).unwrap(),
            to_std_string(refer_info.member->name));
        }
      }

      if(!var_info.has_value() && prefer_native_header)
      {
        var_info = describe_native_candidate();
      }

      if(!var_info.has_value() && !var.is_nil())
      {
        var_info = describe_var(query.target_ns, var, candidate.symbol_name);
      }
      else if(!var_info.has_value() && query.target_ns->name->name == "cpp")
      {
        var_info = describe_cpp_entity(query.target_ns, candidate.symbol_name);
      }
      else if(!var_info.has_value() && query.native_alias.has_value()
              && query.qualifier.has_value())
      {
        var_info = describe_native_candidate();
      }
      if(!var_info.has_value())
      {
        continue;
      }

      bencode::value::dict entry;
      entry.emplace("candidate", candidate.display_name);
      entry.emplace("type", completion_type_for(var_info.value()));
      if(include_ns_info)
      {
        entry.emplace("ns", var_info->ns_name);
      }
      if(include_doc && var_info->doc.has_value())
      {
        entry.emplace("doc", var_info->doc.value());
      }
      if(include_arglists && !var_info->arglists.empty())
      {
        entry.emplace("arglists", bencode::list_of_strings(var_info->arglists));
        if(var_info->arglists_str.has_value())
        {
          entry.emplace("arglists-str", var_info->arglists_str.value());
        }
      }
      completion_payloads.emplace_back(std::move(entry));
    }

    bencode::value::dict payload;
    if(!msg.id().empty())
    {
      payload.emplace("id", msg.id());
    }
    payload.emplace("session", session.id);
    payload.emplace("completions", bencode::value{ std::move(completion_payloads) });
    payload.emplace("status", bencode::list_of_strings({ "done" }));
    return { std::move(payload) };
  }
}
