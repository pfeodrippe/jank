#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

#include <jank/jit/bss_preallocator.hpp>
#include <jank/util/fmt/print.hpp>

namespace jank::jit
{
  bss_preallocator::~bss_preallocator()
  {
    clear();
  }

  bool bss_preallocator::preallocate(llvm::orc::LLJIT &jit,
                                     llvm::orc::JITDylib &dylib,
                                     native_vector<compile_server::constant_info> const &constants)
  {
    if(constants.empty())
    {
      return true;
    }

    auto &es{ jit.getExecutionSession() };
    llvm::orc::SymbolMap symbols;
    llvm::orc::MangleAndInterner interner{ es, jit.getDataLayout() };

    for(auto const &c : constants)
    {
      /* Allocate aligned storage on heap.
       * We use aligned_alloc which guarantees proper alignment for object_ref. */
      void *storage{ nullptr };
#ifdef _WIN32
      storage = _aligned_malloc(c.size, c.alignment);
#else
      if(posix_memalign(&storage, c.alignment, c.size) != 0)
      {
        storage = nullptr;
      }
#endif

      if(storage == nullptr)
      {
        util::println(stderr, "[bss_preallocator] Failed to allocate {} bytes for {}", c.size, c.qualified_name);
        return false;
      }

      /* Zero-initialize (BSS semantics) */
      std::memset(storage, 0, c.size);
      allocations.push_back(storage);

      /* Register symbol with ORC JIT using absoluteSymbols().
       * The mangled name from codegen should match what the cross-compiled object file expects. */
      symbols[interner(c.qualified_name)] = llvm::orc::ExecutorSymbolDef(
        llvm::orc::ExecutorAddr(reinterpret_cast<uintptr_t>(storage)),
        llvm::JITSymbolFlags::Exported);

      util::println(stderr, "[bss_preallocator] Pre-allocated {} at {:p} for symbol {}",
                    c.size, storage, c.qualified_name);
    }

    /* Define all symbols at once in the JITDylib */
    auto err{ dylib.define(llvm::orc::absoluteSymbols(symbols)) };
    if(err)
    {
      util::println(stderr, "[bss_preallocator] Failed to define symbols: {}",
                    llvm::toString(std::move(err)));
      return false;
    }

    util::println(stderr, "[bss_preallocator] Pre-allocated {} constants", constants.size());
    return true;
  }

  void bss_preallocator::clear()
  {
    for(auto *ptr : allocations)
    {
#ifdef _WIN32
      _aligned_free(ptr);
#else
      std::free(ptr);
#endif
    }
    allocations.clear();
  }
}
