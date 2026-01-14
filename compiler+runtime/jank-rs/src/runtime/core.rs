//! Core functions for jank-rs
//!
//! This module implements the standard library functions.

use std::sync::Arc;

use crate::types::{Value, Function, Arity, Symbol};
use crate::error::{JankError, JankResult};
use crate::runtime::env::Environment;

/// Load all core functions into an environment
pub fn load_core(env: &Environment) {
    // Arithmetic
    env.define("+", native_fn("+", core_add, Arity::Variadic(0)));
    env.define("-", native_fn("-", core_sub, Arity::Variadic(1)));
    env.define("*", native_fn("*", core_mul, Arity::Variadic(0)));
    env.define("/", native_fn("/", core_div, Arity::Variadic(1)));
    env.define("mod", native_fn("mod", core_mod, Arity::Fixed(2)));
    env.define("inc", native_fn("inc", core_inc, Arity::Fixed(1)));
    env.define("dec", native_fn("dec", core_dec, Arity::Fixed(1)));

    // Comparison
    env.define("=", native_fn("=", core_eq, Arity::Variadic(1)));
    env.define("not=", native_fn("not=", core_not_eq, Arity::Variadic(1)));
    env.define("<", native_fn("<", core_lt, Arity::Variadic(1)));
    env.define(">", native_fn(">", core_gt, Arity::Variadic(1)));
    env.define("<=", native_fn("<=", core_lte, Arity::Variadic(1)));
    env.define(">=", native_fn(">=", core_gte, Arity::Variadic(1)));

    // Boolean
    env.define("not", native_fn("not", core_not, Arity::Fixed(1)));
    env.define("and", native_fn("and", core_and, Arity::Variadic(0)));
    env.define("or", native_fn("or", core_or, Arity::Variadic(0)));

    // Predicates
    env.define("nil?", native_fn("nil?", core_nil_p, Arity::Fixed(1)));
    env.define("true?", native_fn("true?", core_true_p, Arity::Fixed(1)));
    env.define("false?", native_fn("false?", core_false_p, Arity::Fixed(1)));
    env.define("boolean?", native_fn("boolean?", core_boolean_p, Arity::Fixed(1)));
    env.define("number?", native_fn("number?", core_number_p, Arity::Fixed(1)));
    env.define("integer?", native_fn("integer?", core_integer_p, Arity::Fixed(1)));
    env.define("float?", native_fn("float?", core_float_p, Arity::Fixed(1)));
    env.define("string?", native_fn("string?", core_string_p, Arity::Fixed(1)));
    env.define("symbol?", native_fn("symbol?", core_symbol_p, Arity::Fixed(1)));
    env.define("keyword?", native_fn("keyword?", core_keyword_p, Arity::Fixed(1)));
    env.define("list?", native_fn("list?", core_list_p, Arity::Fixed(1)));
    env.define("vector?", native_fn("vector?", core_vector_p, Arity::Fixed(1)));
    env.define("map?", native_fn("map?", core_map_p, Arity::Fixed(1)));
    env.define("set?", native_fn("set?", core_set_p, Arity::Fixed(1)));
    env.define("fn?", native_fn("fn?", core_fn_p, Arity::Fixed(1)));
    env.define("coll?", native_fn("coll?", core_coll_p, Arity::Fixed(1)));
    env.define("seq?", native_fn("seq?", core_seq_p, Arity::Fixed(1)));
    env.define("empty?", native_fn("empty?", core_empty_p, Arity::Fixed(1)));
    env.define("even?", native_fn("even?", core_even_p, Arity::Fixed(1)));
    env.define("odd?", native_fn("odd?", core_odd_p, Arity::Fixed(1)));
    env.define("zero?", native_fn("zero?", core_zero_p, Arity::Fixed(1)));
    env.define("pos?", native_fn("pos?", core_pos_p, Arity::Fixed(1)));
    env.define("neg?", native_fn("neg?", core_neg_p, Arity::Fixed(1)));

    // Sequences
    env.define("first", native_fn("first", core_first, Arity::Fixed(1)));
    env.define("rest", native_fn("rest", core_rest, Arity::Fixed(1)));
    env.define("next", native_fn("next", core_next, Arity::Fixed(1)));
    env.define("cons", native_fn("cons", core_cons, Arity::Fixed(2)));
    env.define("conj", native_fn("conj", core_conj, Arity::Variadic(1)));
    env.define("concat", native_fn("concat", core_concat, Arity::Variadic(0)));
    env.define("count", native_fn("count", core_count, Arity::Fixed(1)));
    env.define("nth", native_fn("nth", core_nth, Arity::Multi(vec![2, 3])));
    env.define("last", native_fn("last", core_last, Arity::Fixed(1)));
    env.define("butlast", native_fn("butlast", core_butlast, Arity::Fixed(1)));
    env.define("take", native_fn("take", core_take, Arity::Fixed(2)));
    env.define("drop", native_fn("drop", core_drop, Arity::Fixed(2)));
    env.define("reverse", native_fn("reverse", core_reverse, Arity::Fixed(1)));

    // Collections
    env.define("get", native_fn("get", core_get, Arity::Multi(vec![2, 3])));
    env.define("assoc", native_fn("assoc", core_assoc, Arity::Variadic(3)));
    env.define("dissoc", native_fn("dissoc", core_dissoc, Arity::Variadic(2)));
    env.define("keys", native_fn("keys", core_keys, Arity::Fixed(1)));
    env.define("vals", native_fn("vals", core_vals, Arity::Fixed(1)));
    env.define("contains?", native_fn("contains?", core_contains_p, Arity::Fixed(2)));
    env.define("into", native_fn("into", core_into, Arity::Fixed(2)));

    // Constructors
    env.define("list", native_fn("list", core_list, Arity::Variadic(0)));
    env.define("vector", native_fn("vector", core_vector, Arity::Variadic(0)));
    env.define("hash-map", native_fn("hash-map", core_hash_map, Arity::Variadic(0)));
    env.define("hash-set", native_fn("hash-set", core_hash_set, Arity::Variadic(0)));
    env.define("vec", native_fn("vec", core_vec, Arity::Fixed(1)));
    env.define("set", native_fn("set", core_set, Arity::Fixed(1)));

    // Strings
    env.define("str", native_fn("str", core_str, Arity::Variadic(0)));
    env.define("subs", native_fn("subs", core_subs, Arity::Multi(vec![2, 3])));
    env.define("name", native_fn("name", core_name, Arity::Fixed(1)));
    env.define("symbol", native_fn("symbol", core_symbol, Arity::Multi(vec![1, 2])));
    env.define("keyword", native_fn("keyword", core_keyword, Arity::Multi(vec![1, 2])));

    // Functions
    env.define("identity", native_fn("identity", core_identity, Arity::Fixed(1)));
    env.define("constantly", native_fn("constantly", core_constantly, Arity::Fixed(1)));
    env.define("apply", native_fn("apply", core_apply, Arity::Variadic(2)));
    env.define("partial", native_fn("partial", core_partial, Arity::Variadic(1)));
    env.define("comp", native_fn("comp", core_comp, Arity::Variadic(0)));

    // Higher-order functions
    env.define("map", native_fn("map", core_map, Arity::Variadic(2)));
    env.define("filter", native_fn("filter", core_filter, Arity::Fixed(2)));
    env.define("reduce", native_fn("reduce", core_reduce, Arity::Multi(vec![2, 3])));
    env.define("some", native_fn("some", core_some, Arity::Fixed(2)));
    env.define("every?", native_fn("every?", core_every_p, Arity::Fixed(2)));

    // I/O
    env.define("println", native_fn("println", core_println, Arity::Variadic(0)));
    env.define("print", native_fn("print", core_print, Arity::Variadic(0)));
    env.define("pr", native_fn("pr", core_pr, Arity::Variadic(0)));
    env.define("prn", native_fn("prn", core_prn, Arity::Variadic(0)));

    // Misc
    env.define("type", native_fn("type", core_type, Arity::Fixed(1)));
    env.define("assert", native_fn("assert", core_assert, Arity::Multi(vec![1, 2])));
    env.define("range", native_fn("range", core_range, Arity::Multi(vec![0, 1, 2, 3])));
}

/// Helper to create a native function value
fn native_fn(name: &str, func: fn(&[Value]) -> JankResult<Value>, arity: Arity) -> Value {
    Value::Function(Arc::new(Function::native(name, func, arity)))
}

// ============================================================================
// Arithmetic
// ============================================================================

fn core_add(args: &[Value]) -> JankResult<Value> {
    let mut int_sum: i64 = 0;
    let mut float_sum: f64 = 0.0;
    let mut has_float = false;

    for arg in args {
        match arg {
            Value::Integer(n) => {
                if has_float {
                    float_sum += *n as f64;
                } else {
                    int_sum += n;
                }
            }
            Value::Float(f) => {
                if !has_float {
                    float_sum = int_sum as f64;
                    has_float = true;
                }
                float_sum += f;
            }
            _ => return Err(JankError::type_error("number", arg.type_name())),
        }
    }

    if has_float {
        Ok(Value::Float(float_sum))
    } else {
        Ok(Value::Integer(int_sum))
    }
}

fn core_sub(args: &[Value]) -> JankResult<Value> {
    if args.is_empty() {
        return Err(JankError::arity("-", "at least 1", 0));
    }

    if args.len() == 1 {
        // Unary minus
        return match &args[0] {
            Value::Integer(n) => Ok(Value::Integer(-n)),
            Value::Float(f) => Ok(Value::Float(-f)),
            _ => Err(JankError::type_error("number", args[0].type_name())),
        };
    }

    let (first_int, first_float, mut has_float) = match &args[0] {
        Value::Integer(n) => (*n, *n as f64, false),
        Value::Float(f) => (0, *f, true),
        _ => return Err(JankError::type_error("number", args[0].type_name())),
    };

    let mut result_int = first_int;
    let mut result_float = first_float;

    for arg in args.iter().skip(1) {
        match arg {
            Value::Integer(n) => {
                if has_float {
                    result_float -= *n as f64;
                } else {
                    result_int -= n;
                }
            }
            Value::Float(f) => {
                if !has_float {
                    result_float = result_int as f64;
                    has_float = true;
                }
                result_float -= f;
            }
            _ => return Err(JankError::type_error("number", arg.type_name())),
        }
    }

    if has_float {
        Ok(Value::Float(result_float))
    } else {
        Ok(Value::Integer(result_int))
    }
}

fn core_mul(args: &[Value]) -> JankResult<Value> {
    let mut int_prod: i64 = 1;
    let mut float_prod: f64 = 1.0;
    let mut has_float = false;

    for arg in args {
        match arg {
            Value::Integer(n) => {
                if has_float {
                    float_prod *= *n as f64;
                } else {
                    int_prod *= n;
                }
            }
            Value::Float(f) => {
                if !has_float {
                    float_prod = int_prod as f64;
                    has_float = true;
                }
                float_prod *= f;
            }
            _ => return Err(JankError::type_error("number", arg.type_name())),
        }
    }

    if has_float {
        Ok(Value::Float(float_prod))
    } else {
        Ok(Value::Integer(int_prod))
    }
}

fn core_div(args: &[Value]) -> JankResult<Value> {
    if args.is_empty() {
        return Err(JankError::arity("/", "at least 1", 0));
    }

    if args.len() == 1 {
        // 1/x
        return match &args[0] {
            Value::Integer(n) => {
                if *n == 0 {
                    Err(JankError::DivisionByZero)
                } else {
                    Ok(Value::Integer(1 / n))
                }
            }
            Value::Float(f) => {
                if *f == 0.0 {
                    Err(JankError::DivisionByZero)
                } else {
                    Ok(Value::Float(1.0 / f))
                }
            }
            _ => Err(JankError::type_error("number", args[0].type_name())),
        };
    }

    let (first_int, first_float, mut has_float) = match &args[0] {
        Value::Integer(n) => (*n, *n as f64, false),
        Value::Float(f) => (0, *f, true),
        _ => return Err(JankError::type_error("number", args[0].type_name())),
    };

    let mut result_int = first_int;
    let mut result_float = first_float;

    for arg in args.iter().skip(1) {
        match arg {
            Value::Integer(n) => {
                if *n == 0 {
                    return Err(JankError::DivisionByZero);
                }
                if has_float {
                    result_float /= *n as f64;
                } else {
                    result_int /= n;
                }
            }
            Value::Float(f) => {
                if *f == 0.0 {
                    return Err(JankError::DivisionByZero);
                }
                if !has_float {
                    result_float = result_int as f64;
                    has_float = true;
                }
                result_float /= f;
            }
            _ => return Err(JankError::type_error("number", arg.type_name())),
        }
    }

    if has_float {
        Ok(Value::Float(result_float))
    } else {
        Ok(Value::Integer(result_int))
    }
}

fn core_mod(args: &[Value]) -> JankResult<Value> {
    match (&args[0], &args[1]) {
        (Value::Integer(a), Value::Integer(b)) => {
            if *b == 0 {
                Err(JankError::DivisionByZero)
            } else {
                Ok(Value::Integer(a % b))
            }
        }
        (Value::Float(a), Value::Float(b)) => {
            if *b == 0.0 {
                Err(JankError::DivisionByZero)
            } else {
                Ok(Value::Float(a % b))
            }
        }
        (Value::Integer(a), Value::Float(b)) => {
            if *b == 0.0 {
                Err(JankError::DivisionByZero)
            } else {
                Ok(Value::Float(*a as f64 % b))
            }
        }
        (Value::Float(a), Value::Integer(b)) => {
            if *b == 0 {
                Err(JankError::DivisionByZero)
            } else {
                Ok(Value::Float(a % *b as f64))
            }
        }
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

fn core_inc(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Integer(n + 1)),
        Value::Float(f) => Ok(Value::Float(f + 1.0)),
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

fn core_dec(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Integer(n - 1)),
        Value::Float(f) => Ok(Value::Float(f - 1.0)),
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

// ============================================================================
// Comparison
// ============================================================================

fn core_eq(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Ok(Value::Bool(true));
    }
    for i in 1..args.len() {
        if args[i] != args[0] {
            return Ok(Value::Bool(false));
        }
    }
    Ok(Value::Bool(true))
}

fn core_not_eq(args: &[Value]) -> JankResult<Value> {
    match core_eq(args)? {
        Value::Bool(b) => Ok(Value::Bool(!b)),
        _ => unreachable!(),
    }
}

fn compare_numbers(a: &Value, b: &Value) -> JankResult<std::cmp::Ordering> {
    use std::cmp::Ordering;

    match (a, b) {
        (Value::Integer(x), Value::Integer(y)) => Ok(x.cmp(y)),
        (Value::Float(x), Value::Float(y)) => {
            x.partial_cmp(y).ok_or_else(|| JankError::eval("cannot compare NaN"))
        }
        (Value::Integer(x), Value::Float(y)) => {
            (*x as f64).partial_cmp(y).ok_or_else(|| JankError::eval("cannot compare NaN"))
        }
        (Value::Float(x), Value::Integer(y)) => {
            x.partial_cmp(&(*y as f64)).ok_or_else(|| JankError::eval("cannot compare NaN"))
        }
        _ => Err(JankError::type_error("number", a.type_name())),
    }
}

fn core_lt(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Ok(Value::Bool(true));
    }
    for i in 0..args.len() - 1 {
        if compare_numbers(&args[i], &args[i + 1])? != std::cmp::Ordering::Less {
            return Ok(Value::Bool(false));
        }
    }
    Ok(Value::Bool(true))
}

fn core_gt(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Ok(Value::Bool(true));
    }
    for i in 0..args.len() - 1 {
        if compare_numbers(&args[i], &args[i + 1])? != std::cmp::Ordering::Greater {
            return Ok(Value::Bool(false));
        }
    }
    Ok(Value::Bool(true))
}

fn core_lte(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Ok(Value::Bool(true));
    }
    for i in 0..args.len() - 1 {
        if compare_numbers(&args[i], &args[i + 1])? == std::cmp::Ordering::Greater {
            return Ok(Value::Bool(false));
        }
    }
    Ok(Value::Bool(true))
}

fn core_gte(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Ok(Value::Bool(true));
    }
    for i in 0..args.len() - 1 {
        if compare_numbers(&args[i], &args[i + 1])? == std::cmp::Ordering::Less {
            return Ok(Value::Bool(false));
        }
    }
    Ok(Value::Bool(true))
}

// ============================================================================
// Boolean
// ============================================================================

fn core_not(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(!args[0].is_truthy()))
}

fn core_and(args: &[Value]) -> JankResult<Value> {
    for arg in args {
        if !arg.is_truthy() {
            return Ok(arg.clone());
        }
    }
    Ok(args.last().cloned().unwrap_or(Value::Bool(true)))
}

fn core_or(args: &[Value]) -> JankResult<Value> {
    for arg in args {
        if arg.is_truthy() {
            return Ok(arg.clone());
        }
    }
    Ok(args.last().cloned().unwrap_or(Value::Nil))
}

// ============================================================================
// Predicates
// ============================================================================

fn core_nil_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Nil)))
}

fn core_true_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Bool(true))))
}

fn core_false_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Bool(false))))
}

fn core_boolean_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Bool(_))))
}

fn core_number_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Integer(_) | Value::Float(_))))
}

fn core_integer_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Integer(_))))
}

fn core_float_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Float(_))))
}

fn core_string_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::String(_))))
}

fn core_symbol_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Symbol(_))))
}

fn core_keyword_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Keyword(_))))
}

fn core_list_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::List(_))))
}

fn core_vector_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Vector(_))))
}

fn core_map_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Map(_))))
}

fn core_set_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Set(_))))
}

fn core_fn_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::Function(_))))
}

fn core_coll_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(
        args[0],
        Value::List(_) | Value::Vector(_) | Value::Map(_) | Value::Set(_)
    )))
}

fn core_seq_p(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Bool(matches!(args[0], Value::List(_))))
}

fn core_empty_p(args: &[Value]) -> JankResult<Value> {
    let is_empty = match &args[0] {
        Value::Nil => true,
        Value::String(s) => s.is_empty(),
        Value::List(l) => l.is_empty(),
        Value::Vector(v) => v.is_empty(),
        Value::Map(m) => m.is_empty(),
        Value::Set(s) => s.is_empty(),
        _ => return Err(JankError::type_error("collection", args[0].type_name())),
    };
    Ok(Value::Bool(is_empty))
}

fn core_even_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Bool(n % 2 == 0)),
        _ => Err(JankError::type_error("integer", args[0].type_name())),
    }
}

fn core_odd_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Bool(n % 2 != 0)),
        _ => Err(JankError::type_error("integer", args[0].type_name())),
    }
}

fn core_zero_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Bool(*n == 0)),
        Value::Float(f) => Ok(Value::Bool(*f == 0.0)),
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

fn core_pos_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Bool(*n > 0)),
        Value::Float(f) => Ok(Value::Bool(*f > 0.0)),
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

fn core_neg_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Integer(n) => Ok(Value::Bool(*n < 0)),
        Value::Float(f) => Ok(Value::Bool(*f < 0.0)),
        _ => Err(JankError::type_error("number", args[0].type_name())),
    }
}

// ============================================================================
// Sequences
// ============================================================================

fn to_seq(val: &Value) -> JankResult<Vec<Value>> {
    match val {
        Value::Nil => Ok(vec![]),
        Value::List(l) => Ok(l.iter().cloned().collect()),
        Value::Vector(v) => Ok(v.iter().cloned().collect()),
        Value::Map(m) => Ok(m.iter().map(|(k, v)| Value::vector(vec![k.clone(), v.clone()])).collect()),
        Value::Set(s) => Ok(s.iter().cloned().collect()),
        Value::String(s) => Ok(s.chars().map(Value::Char).collect()),
        _ => Err(JankError::type_error("seqable", val.type_name())),
    }
}

fn core_first(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    Ok(seq.into_iter().next().unwrap_or(Value::Nil))
}

fn core_rest(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    Ok(Value::list(seq.into_iter().skip(1)))
}

fn core_next(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    let rest: Vec<_> = seq.into_iter().skip(1).collect();
    if rest.is_empty() {
        Ok(Value::Nil)
    } else {
        Ok(Value::list(rest))
    }
}

fn core_cons(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[1])?;
    let mut result = vec![args[0].clone()];
    result.extend(seq);
    Ok(Value::list(result))
}

fn core_conj(args: &[Value]) -> JankResult<Value> {
    let coll = &args[0];
    let items = &args[1..];

    match coll {
        Value::Nil => Ok(Value::list(items.to_vec())),
        Value::List(l) => {
            let mut result: Vec<Value> = items.iter().rev().cloned().collect();
            result.extend(l.iter().cloned());
            Ok(Value::list(result))
        }
        Value::Vector(v) => {
            let mut result = (**v).clone();
            for item in items {
                result.push_back(item.clone());
            }
            Ok(Value::Vector(Arc::new(result)))
        }
        Value::Set(s) => {
            let mut result = (**s).clone();
            for item in items {
                result.insert(item.clone());
            }
            Ok(Value::Set(Arc::new(result)))
        }
        Value::Map(m) => {
            let mut result = (**m).clone();
            for item in items {
                match item {
                    Value::Vector(kv) if kv.len() == 2 => {
                        result.insert(kv[0].clone(), kv[1].clone());
                    }
                    _ => return Err(JankError::type_error("vector pair", item.type_name())),
                }
            }
            Ok(Value::Map(Arc::new(result)))
        }
        _ => Err(JankError::type_error("collection", coll.type_name())),
    }
}

fn core_concat(args: &[Value]) -> JankResult<Value> {
    let mut result = Vec::new();
    for arg in args {
        result.extend(to_seq(arg)?);
    }
    Ok(Value::list(result))
}

fn core_count(args: &[Value]) -> JankResult<Value> {
    let count = match &args[0] {
        Value::Nil => 0,
        Value::String(s) => s.len(),
        Value::List(l) => l.len(),
        Value::Vector(v) => v.len(),
        Value::Map(m) => m.len(),
        Value::Set(s) => s.len(),
        _ => return Err(JankError::type_error("collection", args[0].type_name())),
    };
    Ok(Value::Integer(count as i64))
}

fn core_nth(args: &[Value]) -> JankResult<Value> {
    let idx = match &args[1] {
        Value::Integer(n) => *n as usize,
        _ => return Err(JankError::type_error("integer", args[1].type_name())),
    };

    let result = match &args[0] {
        Value::Vector(v) => v.get(idx).cloned(),
        Value::List(l) => l.iter().nth(idx).cloned(),
        Value::String(s) => s.chars().nth(idx).map(Value::Char),
        _ => return Err(JankError::type_error("indexed", args[0].type_name())),
    };

    match result {
        Some(v) => Ok(v),
        None if args.len() == 3 => Ok(args[2].clone()),
        None => Err(JankError::IndexOutOfBounds {
            index: idx as i64,
            size: match &args[0] {
                Value::Vector(v) => v.len(),
                Value::List(l) => l.len(),
                Value::String(s) => s.len(),
                _ => 0,
            },
        }),
    }
}

fn core_last(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    Ok(seq.into_iter().last().unwrap_or(Value::Nil))
}

fn core_butlast(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    if seq.is_empty() {
        Ok(Value::Nil)
    } else {
        let len = seq.len();
        Ok(Value::list(seq.into_iter().take(len - 1)))
    }
}

fn core_take(args: &[Value]) -> JankResult<Value> {
    let n = match &args[0] {
        Value::Integer(n) => *n as usize,
        _ => return Err(JankError::type_error("integer", args[0].type_name())),
    };
    let seq = to_seq(&args[1])?;
    Ok(Value::list(seq.into_iter().take(n)))
}

fn core_drop(args: &[Value]) -> JankResult<Value> {
    let n = match &args[0] {
        Value::Integer(n) => *n as usize,
        _ => return Err(JankError::type_error("integer", args[0].type_name())),
    };
    let seq = to_seq(&args[1])?;
    Ok(Value::list(seq.into_iter().skip(n)))
}

fn core_reverse(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    Ok(Value::list(seq.into_iter().rev()))
}

// ============================================================================
// Collections
// ============================================================================

fn core_get(args: &[Value]) -> JankResult<Value> {
    let default = args.get(2).cloned().unwrap_or(Value::Nil);

    match &args[0] {
        Value::Map(m) => Ok(m.get(&args[1]).cloned().unwrap_or(default)),
        Value::Vector(v) => match &args[1] {
            Value::Integer(i) => Ok(v.get(*i as usize).cloned().unwrap_or(default)),
            _ => Ok(default),
        },
        Value::Set(s) => {
            if s.contains(&args[1]) {
                Ok(args[1].clone())
            } else {
                Ok(default)
            }
        }
        Value::Nil => Ok(default),
        _ => Ok(default),
    }
}

fn core_assoc(args: &[Value]) -> JankResult<Value> {
    if args.len() < 3 || (args.len() - 1) % 2 != 0 {
        return Err(JankError::arity("assoc", "odd number >= 3", args.len()));
    }

    match &args[0] {
        Value::Map(m) => {
            let mut result = (**m).clone();
            for chunk in args[1..].chunks(2) {
                result.insert(chunk[0].clone(), chunk[1].clone());
            }
            Ok(Value::Map(Arc::new(result)))
        }
        Value::Vector(v) => {
            let mut result = (**v).clone();
            for chunk in args[1..].chunks(2) {
                let idx = match &chunk[0] {
                    Value::Integer(i) => *i as usize,
                    _ => return Err(JankError::type_error("integer", chunk[0].type_name())),
                };
                if idx >= result.len() {
                    return Err(JankError::IndexOutOfBounds {
                        index: idx as i64,
                        size: result.len(),
                    });
                }
                result.set(idx, chunk[1].clone());
            }
            Ok(Value::Vector(Arc::new(result)))
        }
        Value::Nil => {
            let mut result = imbl::HashMap::new();
            for chunk in args[1..].chunks(2) {
                result.insert(chunk[0].clone(), chunk[1].clone());
            }
            Ok(Value::Map(Arc::new(result)))
        }
        _ => Err(JankError::type_error("map or vector", args[0].type_name())),
    }
}

fn core_dissoc(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Map(m) => {
            let mut result = (**m).clone();
            for key in &args[1..] {
                result.remove(key);
            }
            Ok(Value::Map(Arc::new(result)))
        }
        Value::Nil => Ok(Value::Nil),
        _ => Err(JankError::type_error("map", args[0].type_name())),
    }
}

fn core_keys(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Map(m) => Ok(Value::list(m.keys().cloned().collect::<Vec<_>>())),
        Value::Nil => Ok(Value::Nil),
        _ => Err(JankError::type_error("map", args[0].type_name())),
    }
}

fn core_vals(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Map(m) => Ok(Value::list(m.values().cloned().collect::<Vec<_>>())),
        Value::Nil => Ok(Value::Nil),
        _ => Err(JankError::type_error("map", args[0].type_name())),
    }
}

fn core_contains_p(args: &[Value]) -> JankResult<Value> {
    match &args[0] {
        Value::Map(m) => Ok(Value::Bool(m.contains_key(&args[1]))),
        Value::Set(s) => Ok(Value::Bool(s.contains(&args[1]))),
        Value::Vector(v) => match &args[1] {
            Value::Integer(i) => Ok(Value::Bool(*i >= 0 && (*i as usize) < v.len())),
            _ => Ok(Value::Bool(false)),
        },
        Value::Nil => Ok(Value::Bool(false)),
        _ => Err(JankError::type_error("collection", args[0].type_name())),
    }
}

fn core_into(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[1])?;

    match &args[0] {
        Value::Vector(v) => {
            let mut result = (**v).clone();
            for item in seq {
                result.push_back(item);
            }
            Ok(Value::Vector(Arc::new(result)))
        }
        Value::List(l) => {
            let mut items: Vec<_> = seq.into_iter().rev().collect();
            items.extend(l.iter().cloned());
            Ok(Value::list(items))
        }
        Value::Map(m) => {
            let mut result = (**m).clone();
            for item in seq {
                match item {
                    Value::Vector(kv) if kv.len() == 2 => {
                        result.insert(kv[0].clone(), kv[1].clone());
                    }
                    _ => return Err(JankError::type_error("vector pair", item.type_name())),
                }
            }
            Ok(Value::Map(Arc::new(result)))
        }
        Value::Set(s) => {
            let mut result = (**s).clone();
            for item in seq {
                result.insert(item);
            }
            Ok(Value::Set(Arc::new(result)))
        }
        _ => Err(JankError::type_error("collection", args[0].type_name())),
    }
}

// ============================================================================
// Constructors
// ============================================================================

fn core_list(args: &[Value]) -> JankResult<Value> {
    Ok(Value::list(args.to_vec()))
}

fn core_vector(args: &[Value]) -> JankResult<Value> {
    Ok(Value::vector(args.to_vec()))
}

fn core_hash_map(args: &[Value]) -> JankResult<Value> {
    if args.len() % 2 != 0 {
        return Err(JankError::arity("hash-map", "even number", args.len()));
    }
    let mut map = imbl::HashMap::new();
    for chunk in args.chunks(2) {
        map.insert(chunk[0].clone(), chunk[1].clone());
    }
    Ok(Value::Map(Arc::new(map)))
}

fn core_hash_set(args: &[Value]) -> JankResult<Value> {
    let mut set = imbl::HashSet::new();
    for arg in args {
        set.insert(arg.clone());
    }
    Ok(Value::Set(Arc::new(set)))
}

fn core_vec(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    Ok(Value::vector(seq))
}

fn core_set(args: &[Value]) -> JankResult<Value> {
    let seq = to_seq(&args[0])?;
    let mut set = imbl::HashSet::new();
    for item in seq {
        set.insert(item);
    }
    Ok(Value::Set(Arc::new(set)))
}

// ============================================================================
// Strings
// ============================================================================

fn core_str(args: &[Value]) -> JankResult<Value> {
    let mut result = String::new();
    for arg in args {
        match arg {
            Value::Nil => {}
            Value::String(s) => result.push_str(s),
            other => result.push_str(&other.to_string()),
        }
    }
    Ok(Value::String(Arc::new(result)))
}

fn core_subs(args: &[Value]) -> JankResult<Value> {
    let s = match &args[0] {
        Value::String(s) => s,
        _ => return Err(JankError::type_error("string", args[0].type_name())),
    };

    let start = match &args[1] {
        Value::Integer(n) => *n as usize,
        _ => return Err(JankError::type_error("integer", args[1].type_name())),
    };

    let end = if args.len() == 3 {
        match &args[2] {
            Value::Integer(n) => *n as usize,
            _ => return Err(JankError::type_error("integer", args[2].type_name())),
        }
    } else {
        s.len()
    };

    if start > s.len() || end > s.len() || start > end {
        return Err(JankError::IndexOutOfBounds {
            index: start.max(end) as i64,
            size: s.len(),
        });
    }

    Ok(Value::String(Arc::new(s[start..end].to_string())))
}

fn core_name(args: &[Value]) -> JankResult<Value> {
    let name = match &args[0] {
        Value::Symbol(s) => s.name(),
        Value::Keyword(k) => k.name(),
        Value::String(s) => s.as_str(),
        _ => return Err(JankError::type_error("named", args[0].type_name())),
    };
    Ok(Value::String(Arc::new(name.to_string())))
}

fn core_symbol(args: &[Value]) -> JankResult<Value> {
    match args.len() {
        1 => {
            let name = match &args[0] {
                Value::String(s) => s.as_str(),
                Value::Symbol(s) => return Ok(Value::Symbol(s.clone())),
                _ => return Err(JankError::type_error("string", args[0].type_name())),
            };
            Ok(Value::Symbol(Symbol::new(name)))
        }
        2 => {
            let ns = match &args[0] {
                Value::String(s) => s.as_str(),
                Value::Nil => "",
                _ => return Err(JankError::type_error("string", args[0].type_name())),
            };
            let name = match &args[1] {
                Value::String(s) => s.as_str(),
                _ => return Err(JankError::type_error("string", args[1].type_name())),
            };
            if ns.is_empty() {
                Ok(Value::Symbol(Symbol::new(name)))
            } else {
                Ok(Value::Symbol(Symbol::new(&format!("{}/{}", ns, name))))
            }
        }
        _ => Err(JankError::arity("symbol", "1 or 2", args.len())),
    }
}

fn core_keyword(args: &[Value]) -> JankResult<Value> {
    use crate::types::Keyword;

    match args.len() {
        1 => {
            let name = match &args[0] {
                Value::String(s) => s.as_str(),
                Value::Keyword(k) => return Ok(Value::Keyword(k.clone())),
                Value::Symbol(s) => s.name(),
                _ => return Err(JankError::type_error("string", args[0].type_name())),
            };
            Ok(Value::Keyword(Keyword::new(name)))
        }
        2 => {
            let ns = match &args[0] {
                Value::String(s) => s.as_str(),
                Value::Nil => "",
                _ => return Err(JankError::type_error("string", args[0].type_name())),
            };
            let name = match &args[1] {
                Value::String(s) => s.as_str(),
                _ => return Err(JankError::type_error("string", args[1].type_name())),
            };
            if ns.is_empty() {
                Ok(Value::Keyword(Keyword::new(name)))
            } else {
                Ok(Value::Keyword(Keyword::new(&format!("{}/{}", ns, name))))
            }
        }
        _ => Err(JankError::arity("keyword", "1 or 2", args.len())),
    }
}

// ============================================================================
// Functions
// ============================================================================

fn core_identity(args: &[Value]) -> JankResult<Value> {
    Ok(args[0].clone())
}

fn core_constantly(args: &[Value]) -> JankResult<Value> {
    let value = args[0].clone();
    // Create a function that always returns this value
    // For now, we'll use a partial application trick
    let func = Function::native(
        "constantly-result",
        |_| Ok(Value::Nil), // This will be replaced by the closure
        Arity::Variadic(0),
    );

    // Actually, we need a proper closure here. Let's create a native function
    // that captures the value. Since we can't do that with fn pointers, we'll
    // return a special representation.
    // For a proper implementation, this would need first-class closures.
    // For now, return the value wrapped in a simple function.

    // Workaround: return a list that represents (fn [& _] value)
    Ok(Value::list(vec![
        Value::Symbol(Symbol::new("fn")),
        Value::vector(vec![Value::Symbol(Symbol::new("&")), Value::Symbol(Symbol::new("_"))]),
        Value::list(vec![Value::Symbol(Symbol::new("quote")), value]),
    ]))
}

fn core_apply(args: &[Value]) -> JankResult<Value> {
    if args.len() < 2 {
        return Err(JankError::arity("apply", "at least 2", args.len()));
    }

    let func = &args[0];
    let last_arg = &args[args.len() - 1];
    let last_seq = to_seq(last_arg)?;

    // Combine middle args with last seq
    let mut all_args: Vec<Value> = args[1..args.len() - 1].to_vec();
    all_args.extend(last_seq);

    // Need evaluator to apply - this is a limitation
    // For now, return error suggesting use of direct call
    Err(JankError::not_implemented("apply requires evaluator context"))
}

fn core_partial(args: &[Value]) -> JankResult<Value> {
    if args.is_empty() {
        return Err(JankError::arity("partial", "at least 1", 0));
    }

    let func = match &args[0] {
        Value::Function(f) => Arc::clone(f),
        _ => return Err(JankError::type_error("function", args[0].type_name())),
    };

    let applied_args = args[1..].to_vec();

    Ok(Value::Function(Arc::new(Function::Partial {
        func,
        applied_args,
    })))
}

fn core_comp(args: &[Value]) -> JankResult<Value> {
    if args.is_empty() {
        return Ok(Value::Function(Arc::new(Function::native(
            "identity",
            core_identity,
            Arity::Fixed(1),
        ))));
    }

    let functions: Result<Vec<Arc<Function>>, _> = args
        .iter()
        .map(|arg| match arg {
            Value::Function(f) => Ok(Arc::clone(f)),
            _ => Err(JankError::type_error("function", arg.type_name())),
        })
        .collect();

    Ok(Value::Function(Arc::new(Function::Composed {
        functions: functions?,
    })))
}

// ============================================================================
// Higher-order functions
// ============================================================================

fn core_map(args: &[Value]) -> JankResult<Value> {
    // Note: proper map would need evaluator context for applying the function
    // This is a simplified version that only works with native functions
    Err(JankError::not_implemented("map requires evaluator context - use loop/recur"))
}

fn core_filter(args: &[Value]) -> JankResult<Value> {
    Err(JankError::not_implemented("filter requires evaluator context - use loop/recur"))
}

fn core_reduce(args: &[Value]) -> JankResult<Value> {
    Err(JankError::not_implemented("reduce requires evaluator context - use loop/recur"))
}

fn core_some(args: &[Value]) -> JankResult<Value> {
    Err(JankError::not_implemented("some requires evaluator context - use loop/recur"))
}

fn core_every_p(args: &[Value]) -> JankResult<Value> {
    Err(JankError::not_implemented("every? requires evaluator context - use loop/recur"))
}

// ============================================================================
// I/O
// ============================================================================

fn core_println(args: &[Value]) -> JankResult<Value> {
    let output: Vec<String> = args
        .iter()
        .map(|v| match v {
            Value::String(s) => s.to_string(),
            other => other.to_string(),
        })
        .collect();
    println!("{}", output.join(" "));
    Ok(Value::Nil)
}

fn core_print(args: &[Value]) -> JankResult<Value> {
    let output: Vec<String> = args
        .iter()
        .map(|v| match v {
            Value::String(s) => s.to_string(),
            other => other.to_string(),
        })
        .collect();
    print!("{}", output.join(" "));
    Ok(Value::Nil)
}

fn core_pr(args: &[Value]) -> JankResult<Value> {
    let output: Vec<String> = args.iter().map(|v| v.to_string()).collect();
    print!("{}", output.join(" "));
    Ok(Value::Nil)
}

fn core_prn(args: &[Value]) -> JankResult<Value> {
    let output: Vec<String> = args.iter().map(|v| v.to_string()).collect();
    println!("{}", output.join(" "));
    Ok(Value::Nil)
}

// ============================================================================
// Misc
// ============================================================================

fn core_type(args: &[Value]) -> JankResult<Value> {
    Ok(Value::Symbol(Symbol::new(args[0].type_name())))
}

fn core_assert(args: &[Value]) -> JankResult<Value> {
    let value = &args[0];
    if !value.is_truthy() {
        let msg = if args.len() == 2 {
            match &args[1] {
                Value::String(s) => s.to_string(),
                other => other.to_string(),
            }
        } else {
            format!("Assert failed: {}", value)
        };
        return Err(JankError::AssertionError(msg));
    }
    Ok(Value::Nil)
}

fn core_range(args: &[Value]) -> JankResult<Value> {
    let (start, end, step) = match args.len() {
        0 => {
            // Infinite range - not supported yet
            return Err(JankError::not_implemented("infinite range"));
        }
        1 => {
            let end = match &args[0] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[0].type_name())),
            };
            (0, end, 1)
        }
        2 => {
            let start = match &args[0] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[0].type_name())),
            };
            let end = match &args[1] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[1].type_name())),
            };
            (start, end, 1)
        }
        3 => {
            let start = match &args[0] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[0].type_name())),
            };
            let end = match &args[1] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[1].type_name())),
            };
            let step = match &args[2] {
                Value::Integer(n) => *n,
                _ => return Err(JankError::type_error("integer", args[2].type_name())),
            };
            if step == 0 {
                return Err(JankError::invalid_arg("range", "step cannot be zero"));
            }
            (start, end, step)
        }
        _ => return Err(JankError::arity("range", "0-3", args.len())),
    };

    let mut result = Vec::new();
    let mut i = start;

    if step > 0 {
        while i < end {
            result.push(Value::Integer(i));
            i += step;
        }
    } else {
        while i > end {
            result.push(Value::Integer(i));
            i += step;
        }
    }

    Ok(Value::list(result))
}
