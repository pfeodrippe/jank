#pragma once

#include <jtl/option.hpp>

#include <jank/read/source.hpp>
#include <jank/runtime/object.hpp>

namespace jank::runtime
{
  namespace obj
  {
    using persistent_hash_map_ref = oref<struct persistent_hash_map>;
  }

  object_ref meta(object_ref const m);
  object_ref with_meta(object_ref const o, object_ref const m);
  object_ref with_meta_graceful(object_ref const o, object_ref const m);
  object_ref reset_meta(object_ref const o, object_ref const m);

  read::source meta_source(jtl::option<object_ref> const &o);
  read::source object_source(object_ref const o);
  read::source meta_source_hint(object_ref const meta);
  read::source object_source_hint(object_ref const o);
  obj::persistent_hash_map_ref source_to_meta(read::source const &source);
  obj::persistent_hash_map_ref
  source_to_meta(read::source_position const &start, read::source_position const &end);
  object_ref strip_source_from_meta(object_ref const meta);
  jtl::option<object_ref> strip_source_from_meta_opt(jtl::option<object_ref> const &meta);
  void push_source_hint(read::source const &source);
  void pop_source_hint();
  /* Get current source hint for error reporting. Returns unknown if stack is empty. */
  read::source current_source_hint();

  /* Debug execution trace - stores last N locations visited */
  void debug_trace_push(char const *location, char const *file = nullptr, int line = 0);
  void debug_trace_pop();
  std::string debug_trace_dump();

  /* RAII helper for source hint tracking - use at function entry */
  struct source_hint_guard
  {
    source_hint_guard(jtl::immutable_string const &file,
                      jtl::immutable_string const &module,
                      usize line,
                      usize col)
    {
      read::source_position pos;
      pos.line = line;
      pos.col = col;
      read::source src{ pos };
      src.file = file;
      src.module = module;
      push_source_hint(src);
    }

    ~source_hint_guard()
    {
      pop_source_hint();
    }

    source_hint_guard(source_hint_guard const &) = delete;
    source_hint_guard &operator=(source_hint_guard const &) = delete;
  };

  /* RAII helper for debug trace - simpler than source_hint_guard */
  struct debug_trace_guard
  {
    debug_trace_guard(char const *location, char const *file = nullptr, int line = 0)
    {
      debug_trace_push(location, file, line);
    }

    ~debug_trace_guard()
    {
      debug_trace_pop();
    }

    debug_trace_guard(debug_trace_guard const &) = delete;
    debug_trace_guard &operator=(debug_trace_guard const &) = delete;
  };
}
