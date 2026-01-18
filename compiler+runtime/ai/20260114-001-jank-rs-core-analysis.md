# jank-rs: Core Library Analysis and Design

**Date**: 2026-01-14
**Status**: IN PROGRESS

## Summary

This document analyzes jank's `clojure/core.jank` (~7,864 lines) to design an equivalent `core.jrs` for jank-rs that leverages our existing native Rust functions while providing a familiar Clojure-like API.

## Analysis of jank's clojure/core.jank

### Architecture Overview

jank's core.jank uses a two-layer approach:
1. **Native layer** (`cpp/` interop) - Low-level functions implemented in C++
2. **Pure Clojure layer** - Higher-level functions built on top of natives

Example:
```clojure
;; Native layer
(defn seq [o] (cpp/jank.runtime.seq o))
(defn first [o] (cpp/jank.runtime.first o))

;; Built on natives
(defn ffirst [o] (first (first o)))
(defn second [o] (first (next o)))
```

### Categories of Functions

#### 1. Predicates (~30 functions)
- Type checks: `nil?`, `true?`, `false?`, `number?`, `integer?`, `float?`, `string?`, `symbol?`, `keyword?`, `fn?`, `seq?`, `coll?`, `map?`, `vector?`, `list?`, `set?`
- Value checks: `empty?`, `zero?`, `pos?`, `neg?`, `even?`, `odd?`, `some?`
- Collection checks: `sequential?`, `associative?`, `seqable?`

#### 2. Arithmetic (~20 functions)
- Basic: `+`, `-`, `*`, `/`, `mod`, `rem`, `quot`
- Comparison: `=`, `==`, `not=`, `<`, `>`, `<=`, `>=`
- Utility: `inc`, `dec`, `min`, `max`, `abs`
- Bit ops: `bit-and`, `bit-or`, `bit-xor`, `bit-not`, `bit-shift-left`, `bit-shift-right`

#### 3. Sequences (~50 functions)
- Basic: `seq`, `first`, `rest`, `next`, `cons`, `conj`
- Navigation: `nth`, `last`, `butlast`, `second`, `ffirst`, `nnext`
- Building: `list`, `vector`, `vec`, `hash-map`, `hash-set`, `set`
- Transformation: `map`, `filter`, `reduce`, `take`, `drop`, `concat`, `reverse`
- Lazy: `lazy-seq`, `iterate`, `repeat`, `range`, `cycle`, `repeatedly`
- Partitioning: `partition`, `partition-all`, `partition-by`, `split-at`, `split-with`
- Searching: `some`, `every?`, `not-any?`

#### 4. Collections (~25 functions)
- Access: `get`, `get-in`, `contains?`, `find`
- Modification: `assoc`, `dissoc`, `update`, `update-in`
- Map ops: `keys`, `vals`, `key`, `val`, `select-keys`, `zipmap`, `merge`
- Set ops: `disj`, `union`, `intersection`, `difference` (in clojure.set)

#### 5. Higher-Order Functions (~15 functions)
- Core: `apply`, `partial`, `comp`, `identity`, `constantly`, `complement`
- Composition: `juxt`, `fnil`
- Flow: `some-fn`, `every-pred`

#### 6. Strings (~5 core functions, more in clojure.string)
- Core: `str`, `subs`, `name`, `namespace`
- clojure.string: `join`, `split`, `trim`, `upper-case`, `lower-case`, `replace`, `includes?`, `starts-with?`, `ends-with?`

#### 7. Macros (~40 macros)

**Control Flow:**
- `if`, `when`, `when-not`, `cond`, `condp`, `case`
- `if-let`, `when-let`, `if-some`, `when-some`

**Binding:**
- `let`, `loop`, `fn`, `defn`, `defn-`, `defmacro`, `defonce`
- `binding` (thread-local bindings)

**Threading:**
- `->`, `->>`, `cond->`, `cond->>`, `as->`, `some->`, `some->>`

**Looping:**
- `doseq`, `dotimes`, `while`

**Other:**
- `and`, `or`, `assert`, `comment`, `lazy-seq`, `delay`

#### 8. State Management (~15 functions)
- Atoms: `atom`, `deref`, `reset!`, `swap!`, `compare-and-set!`
- Volatiles: `volatile!`, `vreset!`, `vswap!`
- Delays: `delay`, `force`, `realized?`
- Reduced: `reduced`, `reduced?`, `unreduced`

#### 9. Transients (~8 functions)
- `transient`, `persistent!`, `conj!`, `assoc!`, `dissoc!`, `pop!`, `disj!`

#### 10. Advanced Features
- Multimethods: `defmulti`, `defmethod`, `methods`, `get-method`, `prefer-method`
- Hierarchies: `derive`, `underive`, `isa?`, `parents`, `ancestors`, `descendants`
- Transducers: `transduce`, `eduction`, `cat`
- Metadata: `meta`, `with-meta`, `alter-meta!`, `reset-meta!`

## jank-rs Current State

### Available Native Functions (from core.rs)

**Arithmetic (13):** `+`, `-`, `*`, `/`, `mod`, `inc`, `dec`, `=`, `not=`, `<`, `>`, `<=`, `>=`

**Boolean (3):** `not`, `and`, `or`

**Predicates (23):** `nil?`, `true?`, `false?`, `boolean?`, `number?`, `integer?`, `float?`, `string?`, `symbol?`, `keyword?`, `list?`, `vector?`, `map?`, `set?`, `fn?`, `coll?`, `seq?`, `empty?`, `even?`, `odd?`, `zero?`, `pos?`, `neg?`

**Sequences (14):** `first`, `rest`, `next`, `cons`, `conj`, `concat`, `count`, `nth`, `last`, `butlast`, `take`, `drop`, `reverse`, `range`

**Collections (8):** `get`, `assoc`, `dissoc`, `keys`, `vals`, `contains?`, `into`

**Constructors (6):** `list`, `vector`, `hash-map`, `hash-set`, `vec`, `set`

**Strings (5):** `str`, `subs`, `name`, `symbol`, `keyword`

**Functions (5):** `identity`, `constantly`, `apply`, `partial`, `comp`

**Higher-Order (5):** `map`, `filter`, `reduce`, `some`, `every?` (stub - need evaluator)

**I/O (4):** `println`, `print`, `pr`, `prn`

**Misc (3):** `type`, `assert`, `range`

### Missing from jank-rs

**Critical for core.jrs:**
1. Macros system (defmacro, macro expansion)
2. `apply` (needs evaluator context)
3. `map`, `filter`, `reduce` (needs evaluator context)
4. `gensym`
5. Metadata support

**Nice to have:**
1. `lazy-seq`
2. Threading macros (`->`, `->>`)
3. Transients
4. Atoms/volatiles
5. Multimethods

## Design for core.jrs

### Phase 1: Pure Functions (no macros needed)

Functions that can be defined immediately using existing natives:

```clojure
;; Already in native, re-export for consistency
(def nil? nil?)
(def true? true?)
;; etc.

;; Built on existing functions
(defn second [coll] (first (next coll)))
(defn ffirst [coll] (first (first coll)))
(defn nfirst [coll] (next (first coll)))
(defn fnext [coll] (first (next coll)))
(defn nnext [coll] (next (next coll)))

(defn not-empty [coll]
  (if (empty? coll) nil coll))

(defn min-key [k x & more]
  (reduce (fn [a b] (if (< (k a) (k b)) a b)) x more))

(defn max-key [k x & more]
  (reduce (fn [a b] (if (> (k a) (k b)) a b)) x more))
```

### Phase 2: Control Flow Macros

These expand to special forms we already support:

```clojure
;; when -> if + do
(defmacro when [test & body]
  `(if ~test (do ~@body) nil))

;; when-not -> if + do + not
(defmacro when-not [test & body]
  `(if (not ~test) (do ~@body) nil))

;; cond -> nested if
(defmacro cond [& clauses]
  (when clauses
    `(if ~(first clauses)
       ~(second clauses)
       (cond ~@(nnext clauses)))))
```

### Phase 3: Evaluator-Context Functions

Need to implement these in Rust eval.rs with evaluator access:

```rust
// In eval.rs - implement map/filter/reduce that can call functions
fn eval_map(&mut self, args: &[Value], env: &Environment) -> JankResult<Value>
fn eval_filter(&mut self, args: &[Value], env: &Environment) -> JankResult<Value>
fn eval_reduce(&mut self, args: &[Value], env: &Environment) -> JankResult<Value>
```

### Phase 4: Threading Macros

```clojure
;; -> thread first
(defmacro -> [x & forms]
  (loop [x x forms forms]
    (if forms
      (let [form (first forms)
            threaded (if (seq? form)
                       `(~(first form) ~x ~@(rest form))
                       `(~form ~x))]
        (recur threaded (next forms)))
      x)))

;; ->> thread last
(defmacro ->> [x & forms]
  (loop [x x forms forms]
    (if forms
      (let [form (first forms)
            threaded (if (seq? form)
                       `(~(first form) ~@(rest form) ~x)
                       `(~form ~x))]
        (recur threaded (next forms)))
      x)))
```

## Implementation Plan

### Step 1: Add macro support to jank-rs
1. Add `defmacro` special form
2. Add macro expansion phase before evaluation
3. Add `gensym` for hygienic macros

### Step 2: Fix higher-order functions
1. Implement `map`, `filter`, `reduce`, `some`, `every?` in evaluator
2. Make `apply` work in evaluator context

### Step 3: Create core.jrs with layered approach
1. Re-export native functions
2. Add derived pure functions
3. Add control flow macros
4. Add threading macros

### Step 4: Add jank.string.jrs
- `join`, `split`, `trim`, `upper-case`, `lower-case`

### Step 5: Add jank.set.jrs
- `union`, `intersection`, `difference`, `subset?`, `superset?`

## File Structure

```
src/jank/
├── core.jrs          # Main clojure.core equivalent
├── string.jrs        # String utilities
└── set.jrs           # Set operations
```

## Priority Features for core.jrs v1

**Must Have:**
- [ ] `when`, `when-not`
- [ ] `cond`, `condp`
- [ ] `second`, `ffirst`, `nnext`, etc.
- [ ] `not-empty`
- [ ] Working `map`, `filter`, `reduce`
- [ ] `->`, `->>`

**Nice to Have:**
- [ ] `if-let`, `when-let`
- [ ] `doseq`, `dotimes`
- [ ] `case`
- [ ] `some->`, `some->>`

**Future:**
- [ ] `lazy-seq`
- [ ] Transients
- [ ] Atoms
- [ ] Multimethods

## Next Steps

1. Implement macro system in jank-rs (defmacro, gensym, expand)
2. Fix map/filter/reduce to work with evaluator
3. Create core.jrs with pure functions first
4. Add macros incrementally
5. Add tests for each feature
