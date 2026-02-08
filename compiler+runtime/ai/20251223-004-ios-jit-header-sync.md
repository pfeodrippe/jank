# iOS JIT Header Sync Fix

## Problem
Device JIT was failing with missing headers:
1. `gc/gc_cpp.h` - GC headers missing
2. `immer/heap/heap_policy.hpp` - Wrong directory structure

## Root Cause
The `jank-resources/include/` directory had incorrect structure:
- immer was at `include/immer/immer/heap/...` instead of `include/immer/heap/...`
- Similar issues with folly, bpptree, boost

The headers were copied from repo roots instead of the actual header subdirectories.

## Solution
Added `ios-jit-sync-includes` Makefile target that syncs all third-party headers with correct paths:

```makefile
ios-jit-sync-includes:
    # GC headers from bdwgc
    rsync -av --delete $(JANK_SRC)/third-party/bdwgc/include/gc/ jank-resources/include/gc/
    cp -f $(JANK_SRC)/third-party/bdwgc/include/gc.h jank-resources/include/
    cp -f $(JANK_SRC)/third-party/bdwgc/include/gc_cpp.h jank-resources/include/

    # immer: source is immer/immer/, target is immer/
    rsync -av --delete $(JANK_SRC)/third-party/immer/immer/ jank-resources/include/immer/

    # bpptree
    rsync -av --delete $(JANK_SRC)/third-party/bpptree/include/bpptree/ jank-resources/include/bpptree/

    # folly: source is folly/folly/, target is folly/
    rsync -av --delete $(JANK_SRC)/third-party/folly/folly/ jank-resources/include/folly/

    # boost (merge preprocessor and multiprecision)
    rsync -av $(JANK_SRC)/third-party/boost-preprocessor/include/boost/ jank-resources/include/boost/
    rsync -av $(JANK_SRC)/third-party/boost-multiprecision/include/boost/ jank-resources/include/boost/

    # jank headers
    rsync -av --delete $(JANK_SRC)/include/cpp/jank/ jank-resources/include/jank/
    rsync -av --delete $(JANK_SRC)/include/cpp/jtl/ jank-resources/include/jtl/

    # clojure native headers
    rsync -av --delete $(JANK_SRC)/src/cpp/clojure/ jank-resources/include/clojure/
```

## Header Directory Structure Reference

| Library | Source in jank | Include path expected |
|---------|---------------|----------------------|
| bdwgc | `third-party/bdwgc/include/gc/` | `gc/gc_cpp.h` |
| immer | `third-party/immer/immer/` | `immer/heap/heap_policy.hpp` |
| bpptree | `third-party/bpptree/include/bpptree/` | `bpptree/...` |
| folly | `third-party/folly/folly/` | `folly/SharedMutex.h` |
| boost-preprocessor | `third-party/boost-preprocessor/include/boost/` | `boost/preprocessor/...` |
| boost-multiprecision | `third-party/boost-multiprecision/include/boost/` | `boost/multiprecision/...` |
| jank | `include/cpp/jank/` | `jank/runtime/object.hpp` |

## Note
Both simulator and device JIT now use the same `jank-resources/` directory. The `ios-jit-sync-includes` target is called by both `ios-jit-sim-run` and `ios-jit-device-run`.
