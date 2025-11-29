#include <cstdlib>
#include <exception>
#include <set>
#include <unordered_set>

#include <clang/Interpreter/CppInterOp.h>
#include <Interpreter/Compatibility.h>

/* Include Clang AST headers directly to work around CppInterOp bugs */
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclBase.h>

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

    /* Convert jank-style dot notation to C++ scope notation in a string.
     * e.g., "world.inner" -> "world::inner" */
    std::string dots_to_colons(std::string const &value)
    {
      std::string result;
      result.reserve(value.size() * 2);
      for(auto const ch : value)
      {
        if(ch == '.')
        {
          result.append("::");
        }
        else
        {
          result.push_back(ch);
        }
      }
      return result;
    }

    /* Safely enumerate member function names using Cpp::GetClassMethods.
     * This avoids the problematic declaration chain iteration in GetAllCppNames
     * that can crash on complex classes with mixins.
     *
     * Returns true on success, false if enumeration failed. */
    bool safe_get_class_members(void *scope, std::set<std::string> &names)
    {
      if(!scope)
      {
        return false;
      }

      /* Get class methods - this uses a different iteration path that's safer */
      std::vector<void *> methods;
      Cpp::GetClassMethods(scope, methods);
      for(auto const method : methods)
      {
        if(!method)
        {
          continue;
        }
        auto const name = Cpp::GetFunctionSignature(method);
        if(name.empty())
        {
          continue;
        }
        /* Extract just the function name (before the '(') */
        auto const paren_pos = name.find('(');
        if(paren_pos != std::string::npos)
        {
          auto fn_name = name.substr(0, paren_pos);
          /* Remove any leading return type and class name qualifiers */
          auto const space_pos = fn_name.rfind(' ');
          if(space_pos != std::string::npos)
          {
            fn_name = fn_name.substr(space_pos + 1);
          }
          /* Remove any remaining qualifiers (ClassName::) */
          auto const colon_pos = fn_name.rfind(':');
          if(colon_pos != std::string::npos)
          {
            fn_name = fn_name.substr(colon_pos + 1);
          }
          if(!fn_name.empty() && fn_name[0] != '~')
          {
            names.insert(fn_name);
          }
        }
      }

      return true;
    }

    /* Enumerate members of a nested type within a scope.
     * type_path is the dot-separated path to the type (e.g., "world" or "world.inner")
     * member_prefix is the prefix to filter member names by (can be empty)
     * Returns member names prefixed with the type path for display. */
    std::vector<std::string> enumerate_type_members(std::string const &base_scope_name,
                                                    std::string const &type_path,
                                                    std::string const &member_prefix)
    {
      std::vector<std::string> matches;

      try
      {
        /* Build the full C++ qualified name for the type.
         * e.g., if base_scope_name is "nested_alias_test" and type_path is "world",
         * then full_type_name is "nested_alias_test::world" */
        auto const cpp_type_path = dots_to_colons(type_path);
        std::string full_type_name;
        if(base_scope_name.empty())
        {
          full_type_name = cpp_type_path;
        }
        else
        {
          full_type_name = base_scope_name + "::" + cpp_type_path;
        }

        auto const type_scope = Cpp::GetScopeFromCompleteName(full_type_name);
        if(!type_scope)
        {
          return matches;
        }

        /* Verify it's actually a class/struct or enum */
        if(!Cpp::IsClass(type_scope) && !Cpp::IsEnumScope(type_scope))
        {
          return matches;
        }

        /* Check if the type is complete. Incomplete types (forward declarations,
         * uninstantiated templates) can cause crashes when iterating declarations. */
        if(!Cpp::IsComplete(type_scope))
        {
          return matches;
        }

        /* Get all member names using our safe implementation.
         * This uses GetClassMethods instead of GetAllCppNames,
         * avoiding crashes caused by corrupted declaration chains. */
        std::set<std::string> member_names;
        if(!safe_get_class_members(type_scope, member_names))
        {
          return matches;
        }

        std::unordered_set<std::string> seen;
        seen.reserve(member_names.size());

        for(auto const &name : member_names)
        {
          /* Filter by member prefix if provided */
          if(!member_prefix.empty() && !name.starts_with(member_prefix))
          {
            continue;
          }

          /* Skip compiler-generated names and special members */
          if(name.empty() || name[0] == '~')
          {
            continue;
          }

          /* Check if it's a member function */
          auto const overloads = Cpp::GetFunctionsUsingName(type_scope, name);
          if(!overloads.empty())
          {
            /* Skip constructors (they have the same name as the type) */
            bool is_ctor = false;
            for(auto const fn : overloads)
            {
              if(Cpp::IsConstructor(fn))
              {
                is_ctor = true;
                break;
              }
            }
            if(is_ctor)
            {
              continue;
            }

            if(seen.insert(name).second)
            {
              /* Return as type_path.member_name so display shows full path */
              matches.emplace_back(type_path + "." + name);
            }
            continue;
          }

          /* Check if it's a nested type (class, struct, or enum) */
          auto const child_scope = Cpp::GetScope(name, type_scope);
          if(child_scope && (Cpp::IsClass(child_scope) || Cpp::IsEnumScope(child_scope)))
          {
            if(seen.insert(name).second)
            {
              matches.emplace_back(type_path + "." + name);
            }
          }
        }
      }
      catch(std::exception const &ex)
      {
        util::println(stderr, "enumerate_type_members failed for {}: {}", type_path, ex.what());
      }
      catch(...)
      {
        util::println(stderr, "enumerate_type_members failed for {}: <unknown error>", type_path);
      }

      return matches;
    }
  }

  std::vector<std::string>
  enumerate_native_header_symbols(ns::native_alias const &alias, std::string const &prefix)
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

      /* Check if the prefix contains a dot - this indicates nested member access.
       * e.g., "world." means we want members of the "world" type
       * e.g., "world.defer_" means we want members of "world" starting with "defer_" */
      auto const dot_pos = prefix_name.find('.');
      if(dot_pos != std::string::npos)
      {
        /* Split into type path and member prefix.
         * For "world.defer_": type_path="world", member_prefix="defer_"
         * For "world.": type_path="world", member_prefix="" */
        auto const type_path = prefix_name.substr(0, dot_pos);
        auto const member_prefix = prefix_name.substr(dot_pos + 1);

        return enumerate_type_members(scope_name, type_path, member_prefix);
      }

      /* No dot in prefix - enumerate top-level symbols as before */
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

        // Check if it's a function
        auto const overloads = Cpp::GetFunctionsUsingName(scope_handle, name);
        if(!overloads.empty())
        {
          if(seen.insert(name).second)
          {
            matches.emplace_back(name);
          }
          continue;
        }

        // Check if it's a type (class, struct, or enum)
        auto const child_scope = Cpp::GetScope(name, scope_handle);
        if(child_scope && (Cpp::IsClass(child_scope) || Cpp::IsEnumScope(child_scope)))
        {
          if(seen.insert(name).second)
          {
            matches.emplace_back(name);
          }
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
