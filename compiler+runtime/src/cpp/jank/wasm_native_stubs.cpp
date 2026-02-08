// WASM stubs for native-only functionality
// These functions are called by the runtime but depend on LLVM/JIT which isn't available in WASM
// We provide stub implementations that throw errors if called

#include <jank/util/cli.hpp>

namespace jank::util::cli
{
  // Global opts instance that the runtime references
  // Uses the proper options struct from cli.hpp
  options opts{};
}
