#pragma once

#include <filesystem>
#include <fstream>

#include <jtl/immutable_string.hpp>
#include <jtl/option.hpp>

#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/symbol.hpp>

namespace jank::jit
{
  /* Thread-local state for tracking the current expression hash during JIT compilation.
   * This allows the ObjectLinkingLayer plugin to associate object files with expression hashes. */
  struct jit_cache_context
  {
    /* The hash of the expression currently being compiled. */
    u64 current_hash{};

    /* Whether we're currently compiling a cacheable def. */
    bool is_compiling{};

    /* The C++ declaration string for the current compilation. */
    jtl::immutable_string current_decl_str;

    /* The unique name of the function being compiled. */
    jtl::immutable_string current_unique_name;

    /* The C++ expression string for creating the function instance. */
    jtl::immutable_string current_expr_str;
  };

  /* Get the thread-local JIT cache context. */
  jit_cache_context &get_jit_cache_context();

  /* Persistent JIT cache for storing compiled defs across sessions.
   * Uses the binary version to ensure cache invalidation on version changes.
   *
   * Cache structure:
   *   ~/.cache/jank/<version>/jit_cache/
   *   ├── <hash>.cpp     # C++ source (for debugging/recompilation)
   *   ├── <hash>.meta    # Metadata: qualified_name, unique_name, factory_name
   */
  struct persistent_cache
  {
    persistent_cache(jtl::immutable_string const &binary_version);

    /* Get the cache directory path. */
    std::filesystem::path const &cache_dir() const;

    /* Check if a cached entry exists for the given hash. */
    bool has_cached_source(u64 hash) const;

    /* Save C++ source to disk cache. */
    void save_source(u64 hash,
                     jtl::immutable_string const &cpp_source,
                     jtl::immutable_string const &qualified_name,
                     jtl::immutable_string const &unique_name) const;

    /* Load metadata for a cached entry.
     * Returns none if not found or invalid. */
    struct cache_entry
    {
      jtl::immutable_string qualified_name;
      jtl::immutable_string unique_name;
      jtl::immutable_string cpp_source;
      jtl::immutable_string expression_str;
    };
    jtl::option<cache_entry> load_entry(u64 hash) const;

    /* Check if a compiled object file exists for the given hash. */
    bool has_compiled_object(u64 hash) const;

    /* Get the path to the object file for a hash. */
    std::filesystem::path object_path(u64 hash) const;

    /* Save the expression string for later instantiation. */
    void save_expression(u64 hash, jtl::immutable_string const &expr_str) const;

    /* Compile a cached C++ source to object file using system clang.
     * Returns true on success, false on failure. */
    bool compile_to_object(u64 hash) const;

    /* Get the path to the C++ source file for a hash. */
    std::filesystem::path source_path(u64 hash) const;

    /* Get the path to the expression file for a hash. */
    std::filesystem::path expression_path(u64 hash) const;

    /* Get the factory function name for a hash. */
    static std::string factory_name(u64 hash);

    /* Clear all cached entries. */
    void clear() const;

    /* Get cache statistics. */
    struct stats
    {
      size_t entries{};
      size_t disk_hits{};
      size_t disk_misses{};
    };

    stats get_stats() const;
    void record_disk_hit() const;
    void record_disk_miss() const;

  private:
    std::filesystem::path cache_dir_;
    jtl::immutable_string binary_version_;
    mutable size_t disk_hits_{};
    mutable size_t disk_misses_{};
  };
}
