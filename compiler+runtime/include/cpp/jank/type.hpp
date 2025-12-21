#pragma once

#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <deque>
#include <list>

#include <gc/gc_cpp.h>
#include <gc/gc_allocator.h>

// For WASM, we include immer memory policy components but avoid boost/folly
#include <immer/heap/heap_policy.hpp>
#include <immer/memory_policy.hpp>
#include <jank/runtime/core/jank_heap.hpp>

#if !defined(JANK_TARGET_EMSCRIPTEN) && !defined(JANK_TARGET_IOS)
  #include <boost/multiprecision/cpp_int.hpp>
  #include <boost/multiprecision/cpp_dec_float.hpp>
  #include <folly/FBVector.h>
#endif

#include <jtl/primitive.hpp>

namespace jank
{
  template <typename T>
  using native_allocator = gc_allocator<T>;

  // Same memory policy for both native and WASM - uses jank_heap which
  // respects current_allocator when set, falling back to GC otherwise
  using memory_policy = immer::memory_policy<immer::heap_policy<runtime::jank_heap>,
                                             immer::no_refcount_policy,
                                             immer::default_lock_policy,
                                             immer::gc_transience_policy,
                                             false>;

  using native_persistent_string_view = std::string_view;

#if !defined(JANK_TARGET_EMSCRIPTEN) && !defined(JANK_TARGET_IOS)
  using native_big_integer
    = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<>>;
  using native_big_decimal
    = boost::multiprecision::number<boost::multiprecision::cpp_dec_float<100>,
                                    boost::multiprecision::et_off>;

  template <typename T>
  using native_vector = folly::fbvector<T, native_allocator<T>>;
#else
  // Minimal stubs for WASM/iOS - boost-multiprecision causes math.h macro conflicts
  using native_big_integer = long long; // Stub
  using native_big_decimal = double; // Stub

  template <typename T>
  using native_vector = std::vector<T, native_allocator<T>>;
#endif

  template <typename T>
  using native_deque = std::deque<T, native_allocator<T>>;
  template <typename T>
  using native_list = std::list<T, native_allocator<T>>;
  template <typename K, typename V>
  using native_map = std::map<K, V, std::less<K>, native_allocator<std::pair<K const, V>>>;
  template <typename T>
  using native_set = std::set<T, std::less<T>, native_allocator<T>>;

  template <typename K, typename V, typename Hash = std::hash<K>, typename Pred = std::equal_to<K>>
  using native_unordered_map
    = std::unordered_map<K, V, Hash, Pred, native_allocator<std::pair<K const, V>>>;

  /* TODO: This will leak if it's stored in a GC-tracked object. */
  using native_transient_string = std::string;
}

#include <jank/hash.hpp>

/* NOTE: jtl::immutable_string.hpp includes this file to learn about integer
 * types, but we also include it to forward our string type. Pragma once allows
 * this to work, but we need to make sure the order is right. */
#include <jtl/immutable_string.hpp>
