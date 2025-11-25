#pragma once

// WASM-compatible shim for folly::Synchronized
// WASM is single-threaded, so we don't need actual synchronization

#ifndef JANK_TARGET_EMSCRIPTEN
  #include <folly/Synchronized.h>
#else
  namespace folly
  {
    // Simple passthrough wrapper - no actual synchronization needed in single-threaded WASM
    template <typename T>
    struct Synchronized
    {
      T data;

      Synchronized() = default;
      Synchronized(T const &d) : data{d} {}
      Synchronized(T &&d) : data{std::move(d)} {}

      // Mimic folly's API
      T *wlock() { return &data; }
      T const *rlock() const { return &data; }

      template <typename F>
      auto withWLock(F &&f) -> decltype(f(data))
      {
        return f(data);
      }

      template <typename F>
      auto withRLock(F &&f) const -> decltype(f(data))
      {
        return f(data);
      }
    };
  }
#endif
