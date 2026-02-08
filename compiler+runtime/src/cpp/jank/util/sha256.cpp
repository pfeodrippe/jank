#include <iomanip>

#if defined(JANK_TARGET_IOS)
  #include <CommonCrypto/CommonDigest.h>
#else
  #include <openssl/sha.h>
#endif

#include <jank/util/sha256.hpp>

namespace jank::util
{
  jtl::immutable_string sha256(jtl::immutable_string const &input)
  {
#if defined(JANK_TARGET_IOS)
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> buf{};
    CC_SHA256(reinterpret_cast<unsigned char const *>(input.c_str()),
              static_cast<CC_LONG>(input.size()),
              buf.data());
#else
    std::array<unsigned char, SHA256_DIGEST_LENGTH> buf{};
    SHA256(reinterpret_cast<unsigned char const *>(input.c_str()), input.size(), buf.data());
#endif
    std::stringstream ss;
    for(auto const b : buf)
    {
      ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
  }
}
