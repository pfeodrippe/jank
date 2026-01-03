#pragma once

#include <jank/compile_server/protocol.hpp>
#include <jank/type.hpp>

namespace llvm::orc
{
  class LLJIT;
  class JITDylib;
}

namespace jank::jit
{
  /* Pre-allocates BSS storage for constants and registers them with ORC JIT using absoluteSymbols().
   * This fixes iOS ARM64 JIT ADRP relocation distance violations by ensuring constant storage
   * is at known addresses before object files are loaded. */
  struct bss_preallocator
  {
    bss_preallocator() = default;
    ~bss_preallocator();

    /* Pre-allocate storage and register symbols for the given constants.
     * Must be called BEFORE loading the object file that references these symbols.
     * Returns true on success. */
    bool preallocate(llvm::orc::LLJIT &jit,
                     llvm::orc::JITDylib &dylib,
                     native_vector<compile_server::constant_info> const &constants);

    /* Clear all allocations (called when resetting JIT state). */
    void clear();

  private:
    /* Track allocated memory blocks for cleanup */
    native_vector<void *> allocations;
  };
}
