#include <jank/runtime/behavior/metadatable.hpp>
#include <jank/runtime/core/seq.hpp>
#include <jank/runtime/core/to_string.hpp>
#include <jank/runtime/obj/nil.hpp>
#include <jank/util/fmt.hpp>
#include <iostream>
#include <cstring>

namespace jank::runtime::behavior::detail
{
  object_ref validate_meta(object_ref const m)
  {
    /* Check for corrupted type (valid types are roughly 0-50) */
    if(m.is_some())
    {
      auto const type_val = static_cast<int>(m->type);
      if(type_val < 0 || type_val > 60)
      {
        std::cerr << "[validate_meta] ERROR: corrupted meta type " << type_val
                  << " ptr=" << static_cast<void const *>(m.data) << "\n";
        std::cerr << "[validate_meta] First 16 bytes of object data (hex): ";
        auto const data_ptr = reinterpret_cast<unsigned char const *>(m.data);
        for(int i = 0; i < 16; ++i)
        {
          std::cerr << std::hex << static_cast<int>(data_ptr[i]) << " ";
        }
        std::cerr << std::dec << "\n";
        throw std::runtime_error("[validate_meta] corrupted meta object type: "
                                 + std::to_string(type_val));
      }
    }

    if(!is_map(m) && m.is_some())
    {
      /* Safely get type info without calling to_string which might panic */
      auto const type_str = object_type_str(m->type);
      throw std::runtime_error(std::string("invalid meta type: ") + type_str
                               + " (expected map or nil)");
    }

    return m;
  }
}
