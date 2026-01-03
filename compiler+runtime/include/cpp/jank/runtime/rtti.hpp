#pragma once

#include <jank/runtime/object.hpp>
#include <jank/runtime/core/meta.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/module/loader.hpp>
#include <iostream>
#include <string>
#include <sstream>
#include <execinfo.h>
#include <cxxabi.h>
#include <dlfcn.h>

namespace jank::runtime
{
  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr bool isa(object const * const o)
  {
    jank_debug_assert(o);
    return o->type == T::obj_type;
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> dyn_cast(object_ref const o)
  {
    if(o->type != T::obj_type)
    {
      return {};
    }
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  oref<T> try_object(object_ref const o)
  {
    if(o->type != T::obj_type)
    {
      jtl::string_builder sb;
      sb("invalid object type (expected ");
      sb(object_type_str(T::obj_type));
      sb(" found ");
      sb(object_type_str(o->type));
      sb(")");

      sb("; value=");
      runtime::to_code_string(o, sb);

      auto const source{ runtime::object_source_hint(o) };
      if(source != read::source::unknown())
      {
        sb(" @ ");
        sb(source.to_string());
      }

      // Show current execution context from source hint stack
      auto const current_src{ runtime::current_source_hint() };
      if(current_src != read::source::unknown())
      {
        sb("\n\n=== Current jank Source Location ===\n");
        sb("File: ");
        sb(current_src.file.data());
        sb("\nModule: ");
        sb(current_src.module.data());
        sb("\nLine ");
        sb(std::to_string(current_src.start.line));
        sb(", Column ");
        sb(std::to_string(current_src.start.col));
        if(current_src.end.line != current_src.start.line)
        {
          sb(" to Line ");
          sb(std::to_string(current_src.end.line));
          sb(", Column ");
          sb(std::to_string(current_src.end.col));
        }
        sb("\n");
      }

      // Print debug execution trace
      sb(runtime::debug_trace_dump());

      // Print stack trace for debugging using execinfo (works on iOS)
      sb("\n=== C++ Stack Trace ===\n");
      void *callstack[64];
      int frames = backtrace(callstack, 64);
      char **strs = backtrace_symbols(callstack, frames);
      for(int i = 0; i < frames; ++i)
      {
        std::string frame_str(strs[i]);

        // Try to demangle C++ symbols
        size_t mangled_start = frame_str.find("_Z");
        if(mangled_start != std::string::npos)
        {
          size_t mangled_end = frame_str.find_first_of(" +", mangled_start);
          std::string mangled = frame_str.substr(mangled_start, mangled_end - mangled_start);
          int status = 0;
          char *demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
          if(status == 0 && demangled)
          {
            frame_str.replace(mangled_start, mangled.length(), demangled);
            free(demangled);
          }
        }

#ifdef JANK_IOS_JIT
        // Try to lookup JIT symbol for this address
        uintptr_t frame_addr = reinterpret_cast<uintptr_t>(callstack[i]);
        std::string jit_sym = module::lookup_jit_symbol(frame_addr);
        if(!jit_sym.empty())
        {
          sb("  [JIT] ");
          sb(jit_sym);
          sb(" @ 0x");
          std::ostringstream addr_ss;
          addr_ss << std::hex << frame_addr;
          sb(addr_ss.str());
          sb("\n");
        }
        else
        {
          sb(frame_str);
          sb("\n");
        }
#else
        sb(frame_str);
        sb("\n");
#endif
      }
      free(strs);
      sb("=== End Stack Trace ===\n");

      throw std::runtime_error{ sb.str() };
    }
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  /* This is dangerous. You probably don't want it. Just use `try_object` or `visit_object`.
   * However, if you're absolutely certain you know the type of an erased object, I guess
   * you can use this. */
  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> expect_object(object_ref const o)
  {
    if constexpr(T::obj_type != object_type::nil)
    {
      if(!o.is_some())
      {
        std::cerr << "[EXPECT_OBJECT] null ref for type " << object_type_str(T::obj_type) << "\n";
        throw std::runtime_error("[EXPECT_OBJECT] null ref when expecting type "
                                 + std::string(object_type_str(T::obj_type)));
      }
    }
    if(o->type != T::obj_type)
    {
      std::cerr << "[EXPECT_OBJECT] type mismatch: got " << static_cast<int>(o->type) << " ("
                << object_type_str(o->type) << ")"
                << " expected " << static_cast<int>(T::obj_type) << " ("
                << object_type_str(T::obj_type) << ")"
                << " ptr=" << static_cast<void const *>(o.data) << "\n";

      /* Dump memory around the pointer to help debug ABI mismatches */
      std::cerr << "[EXPECT_OBJECT] Memory dump (first 64 bytes at ptr): ";
      auto const bytes = reinterpret_cast<unsigned char const *>(o.data);
      for(size_t i = 0; i < 64 && bytes; ++i)
      {
        std::cerr << std::hex << static_cast<int>(bytes[i]) << " ";
      }
      std::cerr << std::dec << "\n";

      /* Throw to get a stack trace in debugger */
      throw std::runtime_error(
        "[EXPECT_OBJECT] type mismatch: got " + std::to_string(static_cast<int>(o->type)) + " ("
        + object_type_str(o->type) + ") expected " + std::to_string(static_cast<int>(T::obj_type))
        + " (" + object_type_str(T::obj_type) + ")");
    }
    jank_debug_assert(o->type == T::obj_type);
    return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
  }

  template <typename T>
  requires behavior::object_like<T>
  [[gnu::always_inline, gnu::flatten, gnu::hot]]
  constexpr oref<T> expect_object(object const * const o)
  {
    if(!o)
    {
      std::cerr << "[EXPECT_OBJECT] null pointer for type " << object_type_str(T::obj_type) << "\n";
      throw std::runtime_error("[EXPECT_OBJECT] null pointer when expecting type "
                               + std::string(object_type_str(T::obj_type)));
    }
    if(o->type != T::obj_type)
    {
      std::cerr << "[EXPECT_OBJECT] ptr type mismatch: got " << static_cast<int>(o->type) << " ("
                << object_type_str(o->type) << ")"
                << " expected " << static_cast<int>(T::obj_type) << " ("
                << object_type_str(T::obj_type) << ")\n";
      throw std::runtime_error(
        "[EXPECT_OBJECT] ptr type mismatch: got " + std::to_string(static_cast<int>(o->type)) + " ("
        + object_type_str(o->type) + ") expected " + std::to_string(static_cast<int>(T::obj_type))
        + " (" + object_type_str(T::obj_type) + ")");
    }
    return reinterpret_cast<T *>(reinterpret_cast<char *>(const_cast<object *>(o))
                                 - offsetof(T, base));
  }
}
