# nREPL Server Shutdown Crash Fix

## Problem
When an application using jank's embedded nREPL server is closed, crashes occur during shutdown. Multiple crashes were encountered:

1. **Original crash**: SIGSEGV in `return_single_freelist` (Boehm GC) - IO thread trying to unregister from a partially destroyed GC
2. **Second crash**: SIGSEGV in `std::map::clear()` (PAC failure) - atexit handler racing with static destruction
3. **Third crash**: Same PAC failure - static destruction of `server_registry()` calling map destructor

## Root Cause Analysis

### The Fundamental Problem
During application shutdown (`exit()`), `__cxa_finalize_ranges` runs both:
- `atexit()` handlers
- Static local variable destructors

These interleave in **undefined order**. The `server_registry()` used a static local `std::map` which gets destroyed during this phase. When the map's destructor runs:
1. It calls `clear()` to destroy each `shared_ptr<server>`
2. Server destructors access GC-managed memory
3. The GC may already be partially torn down
4. **CRASH**: Invalid memory access (PAC failure on ARM64)

### Why pthread_detach Wasn't Enough
Even with `pthread_detach` in the server destructor, the map's tree traversal itself crashes because:
- The map nodes may be allocated with memory that's been reclaimed
- The tree structure may be corrupted by other static destructors running first

## Final Solution: Intentionally Leak the Registry

The only reliable fix is to **never destroy the map**:

```cpp
std::map<std::uintptr_t, std::shared_ptr<server>> &server_registry()
{
  /* IMPORTANT: We intentionally heap-allocate and LEAK this map.
   * Using a static local would cause the map's destructor to run during
   * __cxa_finalize_ranges, which crashes because:
   * 1. The destructor calls clear() to destroy each server
   * 2. Server destructors may access GC-managed memory that's already freed
   * 3. Even with pthread_detach, the map tree traversal itself can crash
   *
   * By leaking the map, we avoid the destructor entirely. The process is
   * exiting anyway, so the OS will reclaim all memory. */
  static auto *servers = new std::map<std::uintptr_t, std::shared_ptr<server>>();
  return *servers;
}
```

### Why Leaking is Safe
- The process is exiting - the OS reclaims all memory anyway
- No cleanup is needed since threads are terminated on process exit
- This is a well-known C++ pattern for avoiding static destruction order issues

## Other Changes (Still in Place)
1. **Removed manual GC registration** - Redundant with GC_THREADS
2. **pthread_detach during shutdown** - For cases where servers ARE destroyed
3. **atexit handler** - Sets `shutting_down` flag (now mostly unused but harmless)

## Files Modified
- `compiler+runtime/src/cpp/jank/nrepl_server/asio.cpp`

## Testing
- Ran full test suite: 228 passed, 1 failed (pre-existing unrelated failure)
- No new test regressions introduced

## Key Lesson
When dealing with GC-managed memory and static destruction order:
- Don't try to clean up gracefully during shutdown
- Intentional memory leaks are sometimes the correct solution
- The OS handles cleanup when the process exits
