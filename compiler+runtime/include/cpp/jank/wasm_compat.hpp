#pragma once

// Workaround for emscripten math.h macro conflicts with C++ std functions
#ifdef JANK_TARGET_EMSCRIPTEN
  #include <math.h>
  #ifdef isnan
    #undef isnan
  #endif
  #ifdef isinf
    #undef isinf
  #endif
  #ifdef isfinite
    #undef isfinite
  #endif
  #ifdef isnormal
    #undef isnormal
  #endif
  #ifdef signbit
    #undef signbit
  #endif
  #ifdef isgreater
    #undef isgreater
  #endif
  #ifdef isgreaterequal
    #undef isgreaterequal
  #endif
  #ifdef isless
    #undef isless
  #endif
  #ifdef islessequal
    #undef islessequal
  #endif
  #ifdef islessgreater
    #undef islessgreater
  #endif
  #ifdef isunordered
    #undef isunordered
  #endif
  #ifdef fpclassify
    #undef fpclassify
  #endif
#endif
