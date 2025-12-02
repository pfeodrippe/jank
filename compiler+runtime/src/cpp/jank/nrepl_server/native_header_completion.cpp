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
#include <clang/AST/DeclLookups.h>
#include <clang/Basic/SourceManager.h>

/* Include Clang Preprocessor headers for macro support */
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Basic/IdentifierTable.h>

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
    /* Extract a clean suffix from header_name for comparison.
     * Strips leading ".." and "." path components.
     * e.g., "../test/foo/bar.h" -> "test/foo/bar.h" */
    std::string_view get_clean_header_suffix(std::string_view header_name)
    {
      while(header_name.starts_with("../") || header_name.starts_with("./"))
      {
        if(header_name.starts_with("../"))
        {
          header_name.remove_prefix(3);
        }
        else if(header_name.starts_with("./"))
        {
          header_name.remove_prefix(2);
        }
      }
      return header_name;
    }

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

    /* Check if a declaration is from the specified header file.
     * This is used to filter out symbols from other included headers
     * when doing completion for global scope (C headers).
     *
     * Returns true if:
     * - header_name is empty (no filtering)
     * - the declaration's source file ends with the header name
     * - the declaration is from a built-in or virtual file (always include)
     */
    bool is_decl_from_header(clang::Decl const *decl, std::string_view header_name)
    {
      if(!decl || header_name.empty())
      {
        return true; /* No filtering if no header specified */
      }

      auto &ast_ctx = decl->getASTContext();
      auto &src_mgr = ast_ctx.getSourceManager();
      auto const loc = decl->getLocation();

      if(loc.isInvalid())
      {
        return false; /* Skip invalid locations */
      }

      auto const presumed = src_mgr.getPresumedLoc(loc);
      if(presumed.isInvalid())
      {
        return false;
      }

      std::string_view const filename(presumed.getFilename());

      /* Skip internal Clang interpreter file names */
      if(filename.starts_with("input_line_"))
      {
        return false;
      }

      /* Get clean suffix without relative path components like "../" */
      auto const clean_suffix = get_clean_header_suffix(header_name);

      /* Check if the filename ends with the header name */
      if(filename.ends_with(header_name) || filename.ends_with(clean_suffix))
      {
        return true;
      }

      /* Also check the real path for the file */
      auto const file_id = src_mgr.getFileID(loc);
      if(auto const file_entry = src_mgr.getFileEntryRefForID(file_id))
      {
        auto const real_path = file_entry->getFileEntry().tryGetRealPathName();
        if(!real_path.empty())
        {
          std::string_view const real_path_sv(real_path);
          if(real_path_sv.ends_with(header_name) || real_path_sv.ends_with(clean_suffix))
          {
            return true;
          }
        }

        /* Check the file entry name as well */
        std::string_view const entry_name(file_entry->getName());
        if(entry_name.ends_with(header_name) || entry_name.ends_with(clean_suffix))
        {
          return true;
        }
      }

      return false;
    }

    /* Safely enumerate member names of a class scope using Clang's lookup table.
     * This avoids iterating decls() which can crash on classes with complex
     * declarations (like flecs::world which uses #include inside the class body).
     *
     * Instead, we iterate the lookup table which contains all named members
     * without walking the declaration chain.
     *
     * Returns true on success, false if enumeration failed. */
    bool safe_get_class_members(void *scope, std::set<std::string> &names)
    {
      if(!scope)
      {
        return false;
      }

      try
      {
        auto *decl = static_cast<clang::Decl *>(scope);
        auto *cxxrd = clang::dyn_cast<clang::CXXRecordDecl>(decl);
        if(!cxxrd)
        {
          return false;
        }

        /* Use noload_lookups() to iterate the lookup table without loading
         * external declarations. This is safer than decls() iteration.
         * Pass false to not preserve internal state (we don't need it).
         * We need to iterate using iterators to access getLookupName(). */
        auto lookups = cxxrd->noload_lookups(false);
        for(auto it = lookups.begin(); it != lookups.end(); ++it)
        {
          auto const decl_name = it.getLookupName();
          if(!decl_name.isIdentifier())
          {
            continue;
          }

          auto const name_str = decl_name.getAsString();
          if(name_str.empty() || name_str[0] == '~')
          {
            continue;
          }

          names.insert(name_str);
        }

        return true;
      }
      catch(...)
      {
        /* Swallow all exceptions - just return empty results */
        return false;
      }
    }

    /* Safely enumerate names in a DeclContext (namespace, global scope, etc.)
     * using the lookup table. This is more reliable than iterating decls()
     * because it includes names from #included headers which are added to
     * the lookup table but may not appear in the direct decl iterator.
     *
     * Returns true on success, false if enumeration failed. */
    bool safe_get_decl_context_names(void *scope, std::set<std::string> &names)
    {
      if(!scope)
      {
        return false;
      }

      try
      {
        auto *decl = static_cast<clang::Decl *>(scope);
        auto *dc = clang::dyn_cast<clang::DeclContext>(decl);
        if(!dc)
        {
          return false;
        }

        /* Use noload_lookups() to iterate the lookup table.
         * This finds symbols from #included headers which GetAllCppNames misses. */
        auto lookups = dc->noload_lookups(false);
        for(auto it = lookups.begin(); it != lookups.end(); ++it)
        {
          auto const decl_name = it.getLookupName();
          if(!decl_name.isIdentifier())
          {
            continue;
          }

          auto const name_str = decl_name.getAsString();
          if(name_str.empty() || name_str[0] == '_')
          {
            /* Skip empty names and private/system names starting with _ */
            continue;
          }

          names.insert(name_str);
        }

        return true;
      }
      catch(...)
      {
        /* Swallow all exceptions - just return empty results */
        return false;
      }
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
         * This uses noload_lookups() instead of GetAllCppNames or decls(),
         * avoiding crashes caused by corrupted declaration chains in complex
         * classes like flecs::world that use #include inside the class body. */
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
              std::string full_name;
              full_name.reserve(type_path.size() + 1 + name.size());
              full_name.append(type_path).append(".").append(name);
              matches.emplace_back(std::move(full_name));
            }
            continue;
          }

          /* Check if it's a nested type (class, struct, or enum) */
          auto const child_scope = Cpp::GetScope(name, type_scope);
          if(child_scope && (Cpp::IsClass(child_scope) || Cpp::IsEnumScope(child_scope)))
          {
            if(seen.insert(name).second)
            {
              std::string full_name;
              full_name.reserve(type_path.size() + 1 + name.size());
              full_name.append(type_path).append(".").append(name);
              matches.emplace_back(std::move(full_name));
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

  /* Enumerate class members directly (for class-level scopes).
   * This is used when the scope itself is a class like "flecs::world",
   * allowing `fw/defer_begin` to directly refer to members. */
  static std::vector<std::string>
  enumerate_class_members_directly(void *class_scope, std::string const &prefix)
  {
    std::vector<std::string> matches;

    if(!class_scope)
    {
      return matches;
    }

    /* Note: We don't check IsComplete() here because:
     * 1. safe_get_class_members() has its own exception handling
     * 2. Complex classes like flecs::world may report as incomplete
     *    even when fully included due to template complexity
     * 3. noload_lookups() is safe to call on any CXXRecordDecl */

    /* Get all member names using our safe implementation.
     * This uses noload_lookups() instead of GetAllCppNames or decls(),
     * avoiding crashes caused by corrupted declaration chains in complex
     * classes like flecs::world that use #include inside the class body. */
    std::set<std::string> member_names;
    if(!safe_get_class_members(class_scope, member_names))
    {
      return matches;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(member_names.size());

    for(auto const &name : member_names)
    {
      /* Filter by prefix if provided */
      if(!prefix.empty() && !name.starts_with(prefix))
      {
        continue;
      }

      /* Skip compiler-generated names and special members */
      if(name.empty() || name[0] == '~')
      {
        continue;
      }

      /* Check if it's a member function */
      auto const overloads = Cpp::GetFunctionsUsingName(class_scope, name);
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
          /* Return just the member name (no type path prefix) */
          matches.emplace_back(name);
        }
        continue;
      }

      /* Check if it's a nested type (class, struct, or enum) */
      auto const child_scope = Cpp::GetScope(name, class_scope);
      if(child_scope && (Cpp::IsClass(child_scope) || Cpp::IsEnumScope(child_scope)))
      {
        if(seen.insert(name).second)
        {
          matches.emplace_back(name);
        }
      }
    }

    return matches;
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

      /* Check if the scope itself is a class (not a namespace).
       * This supports class-level scopes like "flecs::world" where
       * fw/defer_begin directly refers to member methods. */
      if(Cpp::IsClass(scope_handle))
      {
        /* Check if the prefix contains a dot - this indicates nested member access.
         * e.g., "inner." means we want members of the nested "inner" type */
        auto const dot_pos = prefix_name.find('.');
        if(dot_pos != std::string::npos)
        {
          /* Split into type path and member prefix.
           * For "inner.foo": type_path="inner", member_prefix="foo" */
          auto const type_path = prefix_name.substr(0, dot_pos);
          auto const member_prefix = prefix_name.substr(dot_pos + 1);

          return enumerate_type_members(scope_name, type_path, member_prefix);
        }

        /* No dot - enumerate class members directly */
        return enumerate_class_members_directly(scope_handle, prefix_name);
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

      /* No dot in prefix - enumerate top-level symbols.
       * We use two approaches and merge the results:
       * 1. GetAllCppNames - iterates the decl list (good for direct declarations)
       * 2. safe_get_decl_context_names - uses lookup tables (finds #included symbols)
       * This combination is needed because GetAllCppNames misses symbols from
       * #included headers (like C functions in raylib.h) but we still want it
       * for any direct declarations. */
      std::set<std::string> candidate_names;
      Cpp::GetAllCppNames(scope_handle, candidate_names);

      /* Also get names from the lookup table - this finds #included C functions */
      safe_get_decl_context_names(scope_handle, candidate_names);

      std::unordered_set<std::string> seen;
      seen.reserve(candidate_names.size());

      /* Get the header name for filtering - only filter for global scope (C headers).
       * For namespaced scopes (C++ headers), we don't filter by header since the
       * namespace already provides the scoping. */
      auto const header_name
        = scope_name.empty() ? std::string(alias.header.begin(), alias.header.end()) : std::string{};

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
          /* For global scope, filter by header file */
          if(!header_name.empty())
          {
            bool found_in_header = false;
            for(auto const fn : overloads)
            {
              if(is_decl_from_header(static_cast<clang::Decl const *>(fn), header_name))
              {
                found_in_header = true;
                break;
              }
            }
            if(!found_in_header)
            {
              continue;
            }
          }

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
          /* For global scope, filter by header file */
          if(!header_name.empty())
          {
            if(!is_decl_from_header(static_cast<clang::Decl const *>(child_scope), header_name))
            {
              continue;
            }
          }

          if(seen.insert(name).second)
          {
            matches.emplace_back(name);
          }
        }
      }

      /* Also enumerate macros from the header.
       * Only for global scope (C headers) since that's where macros are relevant. */
      if(!header_name.empty())
      {
        auto macros = enumerate_native_header_macros(alias, prefix_name);
        for(auto &macro_name : macros)
        {
          if(seen.insert(macro_name).second)
          {
            matches.emplace_back(std::move(macro_name));
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

  namespace
  {
    /* Get the Clang Preprocessor from the CppInterOp interpreter.
     * Returns nullptr if the preprocessor is not available. */
    clang::Preprocessor *get_preprocessor()
    {
      auto *interp = static_cast<compat::Interpreter *>(Cpp::GetInterpreter());
      if(!interp)
      {
        return nullptr;
      }

      auto *ci = interp->getCI();
      if(!ci)
      {
        return nullptr;
      }

      return &ci->getPreprocessor();
    }

    /* Check if a macro was defined in the specified header file.
     * Returns true if the macro's definition location matches the header. */
    bool is_macro_from_header(clang::MacroInfo const *mi, std::string_view header_name)
    {
      if(!mi || header_name.empty())
      {
        return true; /* No filtering if no header specified */
      }

      auto *pp = get_preprocessor();
      if(!pp)
      {
        return false;
      }

      auto &src_mgr = pp->getSourceManager();
      auto const loc = mi->getDefinitionLoc();

      if(loc.isInvalid())
      {
        return false;
      }

      auto const presumed = src_mgr.getPresumedLoc(loc);
      if(presumed.isInvalid())
      {
        return false;
      }

      std::string_view const filename(presumed.getFilename());

      /* Skip internal Clang interpreter file names */
      if(filename.starts_with("input_line_"))
      {
        return false;
      }

      /* Get clean suffix without relative path components like "../" */
      auto const clean_suffix = get_clean_header_suffix(header_name);

      /* Check if the filename ends with the header name */
      if(filename.ends_with(header_name) || filename.ends_with(clean_suffix))
      {
        return true;
      }

      /* Also check the real path for the file */
      auto const file_id = src_mgr.getFileID(loc);
      if(auto const file_entry = src_mgr.getFileEntryRefForID(file_id))
      {
        auto const real_path = file_entry->getFileEntry().tryGetRealPathName();
        if(!real_path.empty())
        {
          std::string_view const real_path_sv(real_path);
          if(real_path_sv.ends_with(header_name) || real_path_sv.ends_with(clean_suffix))
          {
            return true;
          }
        }

        /* Check the file entry name as well */
        std::string_view const entry_name(file_entry->getName());
        if(entry_name.ends_with(header_name) || entry_name.ends_with(clean_suffix))
        {
          return true;
        }
      }

      return false;
    }

    /* Get the string expansion of a macro's tokens.
     * Returns the tokens concatenated with spaces. */
    std::string get_macro_tokens_string(clang::MacroInfo const *mi, clang::Preprocessor &pp)
    {
      std::string result;

      for(auto const &tok : mi->tokens())
      {
        if(!result.empty())
        {
          result += ' ';
        }
        result += pp.getSpelling(tok);
      }

      return result;
    }
  }

  std::vector<std::string>
  enumerate_native_header_macros(ns::native_alias const &alias, std::string const &prefix)
  {
    std::vector<std::string> matches;

    try
    {
      auto *pp = get_preprocessor();
      if(!pp)
      {
        return matches;
      }

      /* Get header name for filtering */
      std::string const header_name(alias.header.begin(), alias.header.end());

      /* Iterate all macros in the preprocessor */
      for(auto const &macro_pair : pp->macros())
      {
        auto const *identifier = macro_pair.first;
        if(!identifier)
        {
          continue;
        }

        auto const name = identifier->getName();

        /* Filter by prefix */
        if(!prefix.empty() && !name.starts_with(prefix))
        {
          continue;
        }

        /* Skip names starting with underscore (internal/system macros) */
        if(name.starts_with("_"))
        {
          continue;
        }

        /* Get the macro definition */
        auto *md = pp->getMacroDefinition(identifier).getMacroInfo();
        if(!md)
        {
          continue;
        }

        /* Filter by header file */
        if(!is_macro_from_header(md, header_name))
        {
          continue;
        }

        matches.emplace_back(name.str());
      }
    }
    catch(...)
    {
      /* Silently ignore errors */
    }

    return matches;
  }

  bool is_native_header_macro(ns::native_alias const &alias, std::string const &name)
  {
    try
    {
      auto *pp = get_preprocessor();
      if(!pp)
      {
        return false;
      }

      /* Check if macro is defined */
      auto *identifier = pp->getIdentifierInfo(name);
      if(!identifier)
      {
        return false;
      }

      auto *md = pp->getMacroDefinition(identifier).getMacroInfo();
      if(!md)
      {
        return false;
      }

      /* Only object-like macros are supported */
      if(md->isFunctionLike())
      {
        return false;
      }

      /* Check if the macro is from the specified header */
      std::string const header_name(alias.header.begin(), alias.header.end());
      return is_macro_from_header(md, header_name);
    }
    catch(...)
    {
      return false;
    }
  }

  bool is_native_header_function_like_macro(ns::native_alias const &alias, std::string const &name)
  {
    try
    {
      auto *pp = get_preprocessor();
      if(!pp)
      {
        return false;
      }

      /* Check if macro is defined */
      auto *identifier = pp->getIdentifierInfo(name);
      if(!identifier)
      {
        return false;
      }

      auto *md = pp->getMacroDefinition(identifier).getMacroInfo();
      if(!md)
      {
        return false;
      }

      /* Only function-like macros */
      if(!md->isFunctionLike())
      {
        return false;
      }

      /* Check if the macro is from the specified header */
      std::string const header_name(alias.header.begin(), alias.header.end());
      return is_macro_from_header(md, header_name);
    }
    catch(...)
    {
      return false;
    }
  }

  std::optional<size_t>
  get_native_header_macro_param_count(ns::native_alias const &alias, std::string const &name)
  {
    try
    {
      auto *pp = get_preprocessor();
      if(!pp)
      {
        return std::nullopt;
      }

      /* Check if macro is defined */
      auto *identifier = pp->getIdentifierInfo(name);
      if(!identifier)
      {
        return std::nullopt;
      }

      auto *md = pp->getMacroDefinition(identifier).getMacroInfo();
      if(!md)
      {
        return std::nullopt;
      }

      /* Only function-like macros */
      if(!md->isFunctionLike())
      {
        return std::nullopt;
      }

      /* Check if the macro is from the specified header */
      std::string const header_name(alias.header.begin(), alias.header.end());
      if(!is_macro_from_header(md, header_name))
      {
        return std::nullopt;
      }

      return md->getNumParams();
    }
    catch(...)
    {
      return std::nullopt;
    }
  }

  std::optional<std::string>
  get_native_header_macro_expansion(ns::native_alias const &alias, std::string const &name)
  {
    try
    {
      auto *pp = get_preprocessor();
      if(!pp)
      {
        return std::nullopt;
      }

      /* Check if macro is defined */
      auto *identifier = pp->getIdentifierInfo(name);
      if(!identifier)
      {
        return std::nullopt;
      }

      auto *md = pp->getMacroDefinition(identifier).getMacroInfo();
      if(!md)
      {
        return std::nullopt;
      }

      /* Check if the macro is from the specified header */
      std::string const header_name(alias.header.begin(), alias.header.end());
      if(!is_macro_from_header(md, header_name))
      {
        return std::nullopt;
      }

      /* Build the expansion string */
      std::string result;

      /* For function-like macros, include the parameter signature */
      if(md->isFunctionLike())
      {
        result += name;
        result += "(";
        bool first = true;
        for(auto const *param : md->params())
        {
          if(!first)
          {
            result += ", ";
          }
          first = false;
          result += param->getName().str();
        }
        result += ") ";
      }

      /* Get the token expansion string */
      result += get_macro_tokens_string(md, *pp);

      return result;
    }
    catch(...)
    {
      return std::nullopt;
    }
  }
}
