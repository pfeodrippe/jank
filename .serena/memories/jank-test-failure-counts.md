# Jank Test Failure Counts

When running jank tests, there are TWO places to check for failure counts:

1. **Jank file tests** - The "files" test case runs many `.jank` test files:
   - Look for: `tested 587 jank files with X skips and Y failures`
   - Baseline: 0 failures

2. **C++ test cases** (doctest) - Traditional C++ unit tests:
   - Look for: `[doctest] test cases: 210 | 209 passed | 1 failed | 0 skipped`
   - Baseline: 1 failure (template function test)

**Always check BOTH counts when running tests!**
