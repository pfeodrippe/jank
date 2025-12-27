#pragma once

#include <jank/analyze/expression.hpp>

namespace jank::analyze
{
  /* Hash an analyzed expression by its structure and values.
   * This excludes source positions so that logically identical
   * expressions produce the same hash regardless of where they
   * appear in source code.
   *
   * Used by the incremental JIT cache to detect unchanged definitions. */
  u64 hash_expression(expression_ref expr);

  /* Combine two hash values using boost::hash_combine algorithm. */
  inline u64 hash_combine(u64 seed, u64 value)
  {
    return seed ^ (value + 0x9e3779b9ULL + (seed << 6) + (seed >> 2));
  }
}
