#pragma once

#include <string>
#include <cstddef>

#if !defined(JANK_TARGET_EMSCRIPTEN)
  #include <cpptrace/cpptrace.hpp>
  #include <cpptrace/basic.hpp>
  #include <cpptrace/from_current.hpp>
  #include <cpptrace/formatting.hpp>
  #include <cpptrace/gdb_jit.hpp>
#else
namespace cpptrace
{
  struct stacktrace_frame
  {
    std::string symbol;
  };

  class stacktrace
  {
  public:
    void print() const
    {
    }
  };

  inline stacktrace generate_trace()
  {
    return {};
  }

  inline stacktrace from_current_exception()
  {
    return {};
  }

  class formatter
  {
  public:
    enum class address_mode
    {
      none
    };

    enum class path_mode
    {
      basename
    };

    formatter &header(char const *)
    {
      return *this;
    }

    formatter &addresses(address_mode)
    {
      return *this;
    }

    formatter &paths(path_mode)
    {
      return *this;
    }

    formatter &columns(bool)
    {
      return *this;
    }

    formatter &snippets(bool)
    {
      return *this;
    }

    formatter &filtered_frame_placeholders(bool)
    {
      return *this;
    }

    template <typename Fn>
    formatter &transform(Fn &&)
    {
      return *this;
    }

    template <typename Fn>
    formatter &filter(Fn &&)
    {
      return *this;
    }

    template <typename Trace>
    void print(Trace const &) const
    {
    }
  };

  namespace detail
  {
    struct jit_debug_descriptor_entry
    {
      void *symfile_addr{};
      std::size_t symfile_size{};
    };

    struct jit_debug_descriptor
    {
      jit_debug_descriptor_entry *relevant_entry{};
    };

    inline jit_debug_descriptor __jit_debug_descriptor{};
  }

  inline void register_jit_object(void const *, std::size_t)
  {
  }
}
#endif
