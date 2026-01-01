// LLVM RTTI stub for iOS JIT builds
//
// When LLVM is built with LLVM_ENABLE_RTTI=OFF but jank is built with RTTI enabled,
// the linker can't find typeinfo for LLVM classes. This stub forces the compiler
// to emit the missing typeinfo.
//
// This is needed because:
// 1. LLVM uses virtual methods in ErrorInfoBase
// 2. jank code references ErrorInfoBase with RTTI
// 3. LLVM was built without RTTI so typeinfo is not in libLLVM*.a

#if defined(JANK_IOS_JIT)

  #include <llvm/Support/Error.h>

namespace
{
  // Force emission of typeinfo for ErrorInfoBase by creating a concrete class
  // that inherits from it and has a key function (first non-inline virtual method)
  class ForceRTTI : public llvm::ErrorInfo<ForceRTTI>
  {
  public:
    static char ID;

    void log(llvm::raw_ostream &) const override
    {
    }

    std::error_code convertToErrorCode() const override
    {
      return std::error_code();
    }
  };

  char ForceRTTI::ID = 0;

  // This function is never called but prevents the compiler from optimizing away
  // the ForceRTTI class and its typeinfo
  [[maybe_unused]]
  void force_rtti_emission()
  {
    ForceRTTI f;
    (void)f;
  }
}

// Explicit instantiation to force typeinfo emission
template class llvm::ErrorInfo<llvm::ErrorList, llvm::ErrorInfoBase>;

#endif // JANK_IOS_JIT
