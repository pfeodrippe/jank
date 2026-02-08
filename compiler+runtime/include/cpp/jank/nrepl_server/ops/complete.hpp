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
    auto const context(msg.get("context"));

    /* Check if we're completing keywords (prefix starts with :) */
    if(!prefix.empty() && prefix[0] == ':')
    {
      bencode::value::list completion_payloads;

      /* Check for auto-resolved keywords (::) */
      bool const is_auto_resolved = prefix.size() >= 2 && prefix[1] == ':';

      if(is_auto_resolved)
      {
        /* For ::foo, complete keywords in the current namespace */
        auto const ns_name(current_ns_name(session.current_ns));
        auto const ns_prefix(ns_name + "/");
        auto const keyword_suffix(prefix.substr(2)); /* Strip leading :: */

        auto const locked_keywords{ __rt_ctx->keywords.rlock() };
        for(auto const &[key, kw] : *locked_keywords)
        {
          auto const key_str(to_std_string(key));
          /* Check if keyword is in the current namespace */
          if(starts_with(key_str, ns_prefix))
          {
            auto const local_name(key_str.substr(ns_prefix.size()));
            /* Check if the local name matches the suffix after :: */
            if(keyword_suffix.empty() || starts_with(local_name, keyword_suffix))
            {
              bencode::value::dict entry;
              entry.emplace("candidate", "::" + local_name);
              entry.emplace("type", "keyword");
              entry.emplace("ns", ns_name);
              completion_payloads.emplace_back(std::move(entry));
            }
          }
        }
      }
      else
      {
        /* For :foo, complete all keywords matching the prefix */
        auto const keyword_prefix(prefix.substr(1)); /* Strip leading : */

        auto const locked_keywords{ __rt_ctx->keywords.rlock() };
        for(auto const &[key, kw] : *locked_keywords)
        {
          auto const key_str(to_std_string(key));
          if(keyword_prefix.empty() || starts_with(key_str, keyword_prefix))
          {
            bencode::value::dict entry;
            entry.emplace("candidate", ":" + key_str);
            entry.emplace("type", "keyword");
            entry.emplace("ns", "");
            completion_payloads.emplace_back(std::move(entry));
          }
        }
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

    /* Check if we're in a require context - if so, provide namespace completions */
    if(is_require_context(context))
    {
      auto const ns_candidates(make_namespace_candidates(prefix));
      bencode::value::list completion_payloads;
      completion_payloads.reserve(ns_candidates.size());
      for(auto const &candidate : ns_candidates)
      {
        bencode::value::dict entry;
        entry.emplace("candidate", candidate.display_name);
        entry.emplace("type", "namespace");
        entry.emplace("ns", "");
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
        return describe_native_header_entity(query.native_alias.value(), candidate.symbol_name);
      };

      // Check if this symbol is a native refer (unqualified symbol referring to a native header entity)
      if(!var_info.has_value() && !query.qualifier.has_value())
      {
        auto const native_refer = query.target_ns->find_native_refer(symbol);
        if(native_refer.is_some())
        {
          auto const &refer_info = native_refer.unwrap();
          var_info = describe_native_header_entity(
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
        var_info = describe_var(var, candidate.symbol_name);
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
