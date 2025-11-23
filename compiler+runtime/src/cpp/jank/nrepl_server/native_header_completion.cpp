#include <cstdlib>
#include <exception>
#include <set>
#include <unordered_set>

#include <clang/Interpreter/CppInterOp.h>
#include <Interpreter/Compatibility.h>

#include <jank/nrepl_server/native_header_completion.hpp>
#include <jank/runtime/context.hpp>
#include <jank/util/fmt/print.hpp>
#include <jank/util/string.hpp>

namespace jank::nrepl_server::asio
{
  using namespace jank;
  using namespace jank::runtime;

  namespace
  {
    std::string to_cpp_scope(jtl::immutable_string const &value)
    {
      std::string scope;
      scope.reserve(value.size() * 2);
      for(auto const ch : value)
      {
        if(ch == '.')
        {
          scope.append("::");
        }
        else
        {
          scope.push_back(ch);
        }
      }
      return scope;
    }
  }

  std::vector<std::string>
  enumerate_native_header_functions(ns::native_alias const &alias, std::string const &prefix)
  {
    std::vector<std::string> matches;

    try
    {
      auto const scope_name = to_cpp_scope(alias.scope);
      auto const prefix_name = prefix.empty() ? std::string{} : prefix;
      auto scope_handle
        = scope_name.empty() ? Cpp::GetGlobalScope() : Cpp::GetScopeFromCompleteName(scope_name);
      if(!scope_handle)
      {
        return matches;
      }

      std::set<std::string> candidate_names;
      Cpp::GetAllCppNames(scope_handle, candidate_names);

      std::unordered_set<std::string> seen;
      seen.reserve(candidate_names.size());

      for(auto const &name : candidate_names)
      {
        if(!prefix_name.empty() && !name.starts_with(prefix_name))
        {
          continue;
        }

        auto const overloads = Cpp::GetFunctionsUsingName(scope_handle, name);
        if(overloads.empty())
        {
          continue;
        }

        if(seen.insert(name).second)
        {
          matches.emplace_back(name);
        }
      }
    }
    catch(std::exception const &ex)
    {
      util::println(stderr, "native header completion failed: {}", ex.what());
    }
    catch(...)
    {
      util::println(stderr, "native header completion failed: <unknown error>");
    }

    return matches;
  }
}
