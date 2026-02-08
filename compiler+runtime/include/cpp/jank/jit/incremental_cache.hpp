#pragma once

#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/symbol.hpp>

namespace jank::runtime
{
  using var_ref = oref<struct var>;
}

namespace jank::jit
{
  /* Stores information about a compiled def to enable incremental JIT.
   * When a def is evaluated with the same body hash, we can skip JIT
   * compilation and reuse the existing var's value. */
  struct compiled_def
  {
    u64 body_hash{};
    runtime::var_ref var{};
  };

  /* Cache for compiled defs. Maps qualified symbol (ns/name) to compiled def info.
   * This enables incremental JIT by skipping recompilation of unchanged defs. */
  struct incremental_cache
  {
    /* Check if a def needs recompilation.
     * Returns true if the def is not in cache or the body hash changed. */
    bool needs_recompile(runtime::obj::symbol_ref qualified_name, u64 body_hash) const
    {
      auto const it = defs.find(qualified_name);
      if(it == defs.end())
      {
        return true;
      }
      return it->second.body_hash != body_hash;
    }

    /* Store a compiled def in the cache. */
    void store(runtime::obj::symbol_ref qualified_name, u64 body_hash, runtime::var_ref var)
    {
      defs[qualified_name] = compiled_def{ body_hash, var };
    }

    /* Get a cached var if the body hash matches.
     * Returns none if not in cache or hash doesn't match. */
    jtl::option<runtime::var_ref> get(runtime::obj::symbol_ref qualified_name, u64 body_hash) const
    {
      auto const it = defs.find(qualified_name);
      if(it == defs.end() || it->second.body_hash != body_hash)
      {
        return jtl::none;
      }
      return it->second.var;
    }

    /* Invalidate a specific def (e.g., for REPL redefinition). */
    void invalidate(runtime::obj::symbol_ref qualified_name)
    {
      defs.erase(qualified_name);
    }

    /* Invalidate all defs in a namespace. */
    void invalidate_namespace(runtime::obj::symbol_ref ns_name)
    {
      for(auto it = defs.begin(); it != defs.end();)
      {
        if(it->first->ns == ns_name->name)
        {
          it = defs.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }

    /* Clear the entire cache. */
    void clear()
    {
      defs.clear();
    }

    /* Get cache statistics for debugging. */
    struct stats
    {
      size_t entries{};
      size_t hits{};
      size_t misses{};
    };

    stats get_stats() const
    {
      return stats{ defs.size(), hits_, misses_ };
    }

    void record_hit() const
    {
      ++hits_;
    }

    void record_miss() const
    {
      ++misses_;
    }

  private:
    native_unordered_map<runtime::obj::symbol_ref, compiled_def> defs;
    mutable size_t hits_{};
    mutable size_t misses_{};
  };
}
