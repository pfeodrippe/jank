#pragma once

#include <folly/Synchronized.h>

#include <jtl/option.hpp>

#include <jank/runtime/var.hpp>
#include <jank/type.hpp>

namespace jank::runtime
{
  struct context;

  namespace obj
  {
    using symbol_ref = oref<struct symbol>;
  }

  using ns_ref = oref<struct ns>;

  struct ns : gc
  {
    static constexpr object_type obj_type{ object_type::ns };
    static constexpr bool pointer_free{ false };

    ns() = delete;
    ns(obj::symbol_ref const name);

    var_ref intern_var(jtl::immutable_string_view const &);
    var_ref intern_var(obj::symbol_ref);
    var_ref intern_owned_var(jtl::immutable_string_view const &);
    var_ref intern_owned_var(obj::symbol_ref);
    var_ref find_var(obj::symbol_ref);
    jtl::result<void, jtl::immutable_string> unmap(obj::symbol_ref sym);

    jtl::result<void, jtl::immutable_string> add_alias(obj::symbol_ref sym, ns_ref ns);
    void remove_alias(obj::symbol_ref sym);
    ns_ref find_alias(obj::symbol_ref sym) const;

    struct native_alias
    {
      jtl::immutable_string header;
      jtl::immutable_string include_directive;
      jtl::immutable_string scope;
    };

    struct native_refer
    {
      obj::symbol_ref alias;
      obj::symbol_ref member;
    };

    jtl::result<bool, jtl::immutable_string>
    add_native_alias(obj::symbol_ref sym, native_alias alias);
    void remove_native_alias(obj::symbol_ref sym);
    jtl::option<native_alias> find_native_alias(obj::symbol_ref sym) const;
    native_vector<native_alias> native_aliases_snapshot() const;

    jtl::result<void, jtl::immutable_string>
    add_native_refer(obj::symbol_ref sym, obj::symbol_ref alias, obj::symbol_ref member);
    void remove_native_refer(obj::symbol_ref sym);
    jtl::option<native_refer> find_native_refer(obj::symbol_ref sym) const;
    native_unordered_map<obj::symbol_ref, native_refer> native_refers_snapshot() const;

    jtl::result<void, jtl::immutable_string> refer(obj::symbol_ref sym, var_ref var);

    obj::persistent_hash_map_ref get_mappings() const;

    /* behavior::object_like */
    bool equal(object const &) const;
    jtl::immutable_string to_string() const;
    jtl::immutable_string to_code_string() const;
    void to_string(jtl::string_builder &buff) const;
    uhash to_hash() const;

    bool operator==(ns const &rhs) const;

    object base{ obj_type };
    obj::symbol_ref name{};
    /* TODO: Benchmark the use of atomics here. That's what Clojure uses. */
    folly::Synchronized<obj::persistent_hash_map_ref> vars;
    folly::Synchronized<obj::persistent_hash_map_ref> aliases;
    folly::Synchronized<native_unordered_map<obj::symbol_ref, native_alias>> native_aliases;
    folly::Synchronized<native_unordered_map<obj::symbol_ref, native_refer>> native_refers;

    std::atomic_uint64_t symbol_counter{};
  };
}
