#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <ranges>
#include <string_view>
#include <variant>
#include <vector>

#include <jank/nrepl_server/engine.hpp>
#include <jank/nrepl_server/native_header_completion.hpp>

/* This must go last; doctest and glog both define CHECK and family. */
#include <doctest/doctest.h>

namespace jank::nrepl_server::asio
{
  inline message make_message(std::initializer_list<std::pair<std::string, std::string>> fields)
  {
    bencode::value::dict dict;
    for(auto const &entry : fields)
    {
      dict.emplace(entry.first, bencode::value{ entry.second });
    }
    return message{ std::move(dict) };
  }

  inline message make_middleware_message(std::string op,
                                         std::vector<std::string> middleware,
                                         std::optional<std::string> session = std::nullopt)
  {
    bencode::value::dict dict;
    dict.emplace("op", bencode::value{ std::move(op) });
    if(session.has_value())
    {
      dict.emplace("session", bencode::value{ session.value() });
    }

    bencode::value::list list;
    list.reserve(middleware.size());
    for(auto &entry : middleware)
    {
      list.emplace_back(std::move(entry));
    }
    dict.emplace("middleware", bencode::value{ std::move(list) });

    return message{ std::move(dict) };
  }

  inline std::vector<std::string> extract_status(bencode::value::dict const &payload)
  {
    std::vector<std::string> statuses;
    auto const status_iter(payload.find("status"));
    if(status_iter == payload.end())
    {
      return statuses;
    }

    auto const &list(status_iter->second.as_list());
    statuses.reserve(list.size());
    for(auto const &entry : list)
    {
      statuses.push_back(entry.as_string());
    }
    return statuses;
  }

  constexpr std::array<std::string_view, 19> expected_ops{ "clone",
                                                           "describe",
                                                           "ls-sessions",
                                                           "close",
                                                           "eval",
                                                           "load-file",
                                                           "completions",
                                                           "complete",
                                                           "lookup",
                                                           "info",
                                                           "eldoc",
                                                           "forward-system-output",
                                                           "interrupt",
                                                           "ls-middleware",
                                                           "add-middleware",
                                                           "swap-middleware",
                                                           "stdin",
                                                           "caught",
                                                           "analyze-last-stacktrace" };

  constexpr std::array<std::string_view, 10> expected_middleware_stack{
    "nrepl.middleware.session/session",
    "nrepl.middleware.caught/wrap-caught",
    "nrepl.middleware.print/wrap-print",
    "nrepl.middleware.interruptible-eval/interruptible-eval",
    "nrepl.middleware.load-file/wrap-load-file",
    "nrepl.middleware.completion/wrap-completion",
    "nrepl.middleware.lookup/wrap-lookup",
    "nrepl.middleware.dynamic-loader/wrap-dynamic-loader",
    "nrepl.middleware.io/wrap-out",
    "nrepl.middleware.session/add-stdin"
  };

  inline std::string dict_keys(bencode::value::dict const &dict)
  {
    std::string joined;
    for(auto const &entry : dict)
    {
      if(!joined.empty())
      {
        joined += ',';
      }
      joined += entry.first;
    }
    return joined;
  }
}
