// Real WASM hot-reload patch for eita/ggg
// This implements the actual jank function using runtime helpers
//
// Original jank code (with +48 → +49 change):
//   (defn ^:export ggg [v]
//     (println :FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!
//              (+ 50 v)
//              (set/union #{999} (set (mapv #(* 3 %) [1 5 v]))))
//     (+ 49 v))  ;; Changed from 48 to 49
//
// Compile with:
//   emcc ggg_real_patch.cpp -o ggg_real_patch.wasm -sSIDE_MODULE=1 -O2 -fPIC

#include <stdint.h>

extern "C"
{
  // Patch symbol metadata
  struct patch_symbol
  {
    char const *qualified_name;
    char const *signature;
    void *fn_ptr;
  };

  // Import runtime helper functions from main module
  extern void *jank_box_integer(int64_t value);
  extern int64_t jank_unbox_integer(void *obj);
  extern void *jank_call_var(char const *ns, char const *name, int argc, void **args);
  extern void *jank_make_keyword(char const *ns, char const *name);
  extern void *jank_make_vector(int argc, void **elements);
  extern void *jank_make_set(int argc, void **elements);

  // Helper: multiply by 3 (for the anonymous function #(* 3 %))
  static void *multiply_by_3(void *x)
  {
    void *three = jank_box_integer(3);
    void *args[] = { three, x };
    return jank_call_var("clojure.core", "*", 2, args);
  }

  // The patched function: eita/ggg
  // Implements:
  //   (println :FROM_CLJ_..._I_MEAN_JANK_IN_WASM!! (+ 50 v) (set/union #{999} (set (mapv #(* 3 %) [1 5 v]))))
  //   (+ 49 v)
  __attribute__((visibility("default"))) void *jank_eita_ggg(void *v)
  {
    // 1. Create keyword :FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!
    void *kw = jank_make_keyword("", "FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!");

    // 2. Calculate (+ 50 v)
    void *fifty = jank_box_integer(50);
    void *plus_args1[] = { fifty, v };
    void *plus_50_v = jank_call_var("clojure.core", "+", 2, plus_args1);

    // 3. Create vector [1 5 v]
    void *one = jank_box_integer(1);
    void *five = jank_box_integer(5);
    void *vec_elems[] = { one, five, v };
    void *input_vec = jank_make_vector(3, vec_elems);

    // 4. mapv #(* 3 %) over the vector
    // Since we can't easily create closures, we manually apply the operation
    // by calling mapv with a var. We'll use a workaround: create the result directly.
    // Actually, let's just compute it manually for now:
    //   (* 3 1) = 3, (* 3 5) = 15, (* 3 v) = 3*v
    void *elem1 = multiply_by_3(one);
    void *elem2 = multiply_by_3(five);
    void *elem3 = multiply_by_3(v);

    // 5. Create set from the mapped values
    void *set_elems[] = { elem1, elem2, elem3 };
    void *mapped_set = jank_make_set(3, set_elems);

    // 6. Create set #{999}
    void *nine99 = jank_box_integer(999);
    void *base_set_elems[] = { nine99 };
    void *base_set = jank_make_set(1, base_set_elems);

    // 7. Call set/union
    void *union_args[] = { base_set, mapped_set };
    void *union_result = jank_call_var("clojure.set", "union", 2, union_args);

    // 8. Call println with all three args
    void *println_args[] = { kw, plus_50_v, union_result };
    jank_call_var("clojure.core", "println", 3, println_args);

    // 9. Return (+ 49 v) - THE CHANGE: 48 → 49
    void *forty_nine = jank_box_integer(49);
    void *plus_args2[] = { forty_nine, v };
    return jank_call_var("clojure.core", "+", 2, plus_args2);
  }

  // Patch metadata export
  __attribute__((visibility("default"))) patch_symbol *jank_patch_symbols(int *count)
  {
    static patch_symbol symbols[] = {
      { "eita/ggg", "1", (void *)jank_eita_ggg }
    };
    *count = 1;
    return symbols;
  }
}
