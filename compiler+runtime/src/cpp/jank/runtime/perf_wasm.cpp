// WASM stub for perf.cpp - nanobench is not available for WASM

#include <jank/runtime/perf.hpp>
#include <jank/runtime/obj/nil.hpp>

namespace jank::runtime::perf
{
  object_ref benchmark(object_ref const /* opts */, object_ref const /* f */)
  {
    // Benchmarking is not supported in WASM - just return nil
    return jank_nil();
  }
}
