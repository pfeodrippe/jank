#include <jank/runtime/ns.hpp>
#include <jank/runtime/rtti.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/obj/persistent_hash_map.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/util/fmt/print.hpp>

namespace jank::runtime
{
  ns::ns(obj::symbol_ref const name)
    : name{ name }
    , vars{ obj::persistent_hash_map::empty() }
    , aliases{ obj::persistent_hash_map::empty() }
  {
  }

  var_ref ns::intern_var(jtl::immutable_string_view const &name)
  {
    return intern_var(make_box<obj::symbol>(name));
  }

  var_ref ns::intern_var(obj::symbol_ref const sym)
  {
    obj::symbol_ref unqualified_sym{ sym };
    if(!unqualified_sym->ns.empty())
    {
      unqualified_sym = make_box<obj::symbol>("", sym->name);
    }

    /* TODO: Read lock, then upgrade as needed? Benchmark. */
    auto locked_vars(vars.wlock());
    object_ref const * const found_var((*locked_vars)->data.find(unqualified_sym));
    if(found_var && found_var->is_some())
    {
      /* TODO: Why not store var_ref instead? Relying on expect_object is not good. */
      return expect_object<var>(*found_var);
    }

    auto const new_var(make_box<var>(this, unqualified_sym));
    *locked_vars
      = make_box<obj::persistent_hash_map>((*locked_vars)->data.set(unqualified_sym, new_var));
    return new_var;
  }

  var_ref ns::intern_owned_var(jtl::immutable_string_view const &name)
  {
    return intern_owned_var(make_box<obj::symbol>(name));
  }

  /* Interning an owned var is different from just interning a var, since we will replace
   * any existing referred var with our own. This leads to a warning. For example:
   *
   * ```
   * user=> (ns foo)
   * nil
   *
   * foo=> (defn println [])
   * WARNING 'println' already referred to #'clojure.core/println in namespace 'foo' but has been replaced by #'foo/println
   * ```
   */
  var_ref ns::intern_owned_var(obj::symbol_ref const sym)
  {
    obj::symbol_ref unqualified_sym{ sym };
    if(!unqualified_sym->ns.empty())
    {
      unqualified_sym = make_box<obj::symbol>("", sym->name);
    }

    /* TODO: Read lock, then upgrade as needed? Benchmark. */
    auto locked_vars(vars.wlock());
    object_ref const * const found_var((*locked_vars)->data.find(unqualified_sym));
    bool redefined{};
    if(found_var && found_var->is_some())
    {
      auto const v{ expect_object<var>(*found_var) };
      if(v->n == this)
      {
        return v;
      }

      redefined = true;
    }

    auto const new_var(make_box<var>(this, unqualified_sym));
    if(redefined)
    {
      auto const v{ expect_object<var>(*found_var) };
      /* TODO: Util for warning. */
      util::println(
        stderr,
        "WARNING: '{}' already referred to {} in namespace '{}' but has been replaced by {}",
        unqualified_sym->to_string(),
        v->to_code_string(),
        name->to_string(),
        new_var->to_code_string());
    }
    *locked_vars
      = make_box<obj::persistent_hash_map>((*locked_vars)->data.set(unqualified_sym, new_var));
    return new_var;
  }

  jtl::result<void, jtl::immutable_string> ns::unmap(obj::symbol_ref const sym)
  {
    if(!sym->ns.empty())
    {
      return err(util::format("Can't unintern namespace-qualified symbol: {}", sym->to_string()));
    }

    auto locked_vars(vars.wlock());
    *locked_vars = make_box<obj::persistent_hash_map>((*locked_vars)->data.erase(sym));
    return ok();
  }

  var_ref ns::find_var(obj::symbol_ref const sym)
  {
    if(!sym->ns.empty())
    {
      return __rt_ctx->find_var(sym);
    }

    auto const locked_vars(vars.rlock());
    auto const found((*locked_vars)->data.find(sym));
    if(!found)
    {
      return {};
    }

    return { expect_object<var>(*found) };
  }

  jtl::result<void, jtl::immutable_string>
  ns::add_alias(obj::symbol_ref const sym, ns_ref const nsp)
  {
    auto locked_aliases(aliases.wlock());
    auto const found((*locked_aliases)->data.find(sym));
    if(found)
    {
      auto const existing(expect_object<ns>(*found));
      if(existing != nsp)
      {
        return err(util::format("{} already aliases {} in ns {}, cannot change to {}",
                                sym->to_string(),
                                existing->to_string(),
                                to_string(),
                                nsp->to_string()));
      }
      return ok();
    }
    *locked_aliases = make_box<obj::persistent_hash_map>((*locked_aliases)->data.set(sym, nsp));
    return ok();
  }

  void ns::remove_alias(obj::symbol_ref const sym)
  {
    auto locked_aliases(aliases.wlock());
    *locked_aliases = make_box<obj::persistent_hash_map>((*locked_aliases)->data.erase(sym));
  }

  ns_ref ns::find_alias(obj::symbol_ref const sym) const
  {
    auto locked_aliases(aliases.rlock());
    auto const found((*locked_aliases)->data.find(sym));
    if(found)
    {
      return expect_object<ns>(*found);
    }
    return {};
  }

  jtl::result<bool, jtl::immutable_string>
  ns::add_native_alias(obj::symbol_ref const sym, native_alias alias)
  {
    auto locked_native_aliases(native_aliases.wlock());
    auto const found(locked_native_aliases->find(sym));
    if(found != locked_native_aliases->end())
    {
      auto const &existing(found->second);
      if(existing.header != alias.header || existing.scope != alias.scope
         || existing.include_directive != alias.include_directive)
      {
        return err(util::format("{} already bound to native header '{}' in ns {}",
                                sym->to_string(),
                                existing.header,
                                to_string()));
      }
      return ok(false);
    }
    locked_native_aliases->emplace(sym, std::move(alias));
    return ok(true);
  }

  void ns::remove_native_alias(obj::symbol_ref const sym)
  {
    auto locked_native_aliases(native_aliases.wlock());
    locked_native_aliases->erase(sym);
  }

  jtl::option<ns::native_alias> ns::find_native_alias(obj::symbol_ref const sym) const
  {
    auto locked_native_aliases(native_aliases.rlock());
    auto const found(locked_native_aliases->find(sym));
    if(found != locked_native_aliases->end())
    {
      return found->second;
    }
    return none;
  }

  native_vector<ns::native_alias> ns::native_aliases_snapshot() const
  {
    native_vector<native_alias> ret;
    auto locked_native_aliases(native_aliases.rlock());
    ret.reserve(locked_native_aliases->size());
    for(auto const &entry : *locked_native_aliases)
    {
      ret.emplace_back(entry.second);
    }
    return ret;
  }

  jtl::result<void, jtl::immutable_string> ns::add_native_refer(obj::symbol_ref const sym,
                                                                obj::symbol_ref const alias_sym,
                                                                obj::symbol_ref const member)
  {
    auto locked_native_refers(native_refers.wlock());
    auto const found(locked_native_refers->find(sym));
    if(found != locked_native_refers->end())
    {
      auto const &existing(found->second);
      // Use value comparison for symbols instead of pointer comparison
      // If the member is different, throw an error
      if(!existing.member->equal(*member))
      {
        return err(util::format("{} already refers to native member '{}.{}' in ns {}",
                                sym->to_string(),
                                existing.alias->to_string(),
                                existing.member->to_string(),
                                to_string()));
      }
      // If member is the same but alias is different, update to use the new alias
      if(!existing.alias->equal(*alias_sym))
      {
        locked_native_refers->at(sym) = native_refer{ alias_sym, member };
      }
      return ok();
    }
    locked_native_refers->emplace(sym, native_refer{ alias_sym, member });
    return ok();
  }

  void ns::remove_native_refer(obj::symbol_ref const sym)
  {
    auto locked_native_refers(native_refers.wlock());
    locked_native_refers->erase(sym);
  }

  jtl::option<ns::native_refer> ns::find_native_refer(obj::symbol_ref const sym) const
  {
    auto locked_native_refers(native_refers.rlock());
    auto const found(locked_native_refers->find(sym));
    if(found != locked_native_refers->end())
    {
      return found->second;
    }
    return none;
  }

  jtl::result<void, jtl::immutable_string> ns::refer(obj::symbol_ref const sym, var_ref const var)
  {
    auto const clojure_core(__rt_ctx->find_ns(make_box<obj::symbol>("clojure.core")));
    auto locked_vars(vars.wlock());
    if(auto const found = (*locked_vars)->data.find(sym))
    {
      if(found->data->type == object_type::var)
      {
        auto const found_var(expect_object<runtime::var>(found->data));
        if(var->n != found_var->n && (found_var->n != clojure_core))
        {
          return err(util::format("{} already refers to {} in ns {}",
                                  sym->to_string(),
                                  expect_object<runtime::var>(*found)->to_string(),
                                  to_string()));
        }
      }
    }
    *locked_vars = make_box<obj::persistent_hash_map>((*locked_vars)->data.set(sym, var));
    return ok();
  }

  obj::persistent_hash_map_ref ns::get_mappings() const
  {
    auto const locked_vars(vars.rlock());
    return *locked_vars;
  }

  bool ns::equal(object const &o) const
  {
    if(o.type != object_type::ns)
    {
      return false;
    }

    auto const v(expect_object<ns>(&o));
    return name == v->name;
  }
  jtl::immutable_string ns::to_string() const
  /* TODO: Maybe cache this. */
  {
    return name->to_string();
  }

  jtl::immutable_string ns::to_code_string() const
  {
    return to_string();
  }

  void ns::to_string(jtl::string_builder &buff) const
  {
    name->to_string(buff);
  }
  uhash ns::to_hash() const
  /* TODO: Cache this? */
  {
    return static_cast<uhash>(reinterpret_cast<uintptr_t>(this));
  }

  bool ns::operator==(ns const &rhs) const
  {
    return name == rhs.name;
  }
}
