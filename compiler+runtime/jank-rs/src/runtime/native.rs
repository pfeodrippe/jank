//! Native Rust functions callable from jank-rs JIT code
//!
//! These functions are registered with the JIT compiler and can be called
//! directly from jank-rs code with zero FFI overhead.
//!
//! All functions use extern "C" calling convention for compatibility with Cranelift.
//!
//! ## Tagged Value Functions (u64)
//! Functions prefixed with `jank_tagged_` work with NaN-boxed tagged u64 values.
//! These handle mixed types (integers, floats, strings, collections, etc.)
//!
//! ## Numeric Functions (i64)
//! Functions like `jank_sqrt`, `jank_abs` work with raw i64 for fast numeric code.

use crate::runtime::tagged::{Tagged, NIL, TRUE, FALSE};

// ============================================================================
// Math Functions (i64 → i64)
// ============================================================================

/// Square root (integer approximation)
#[no_mangle]
pub extern "C" fn jank_sqrt(x: i64) -> i64 {
    if x < 0 {
        0 // Return 0 for negative (could be NaN in future)
    } else {
        (x as f64).sqrt() as i64
    }
}

/// Absolute value
#[no_mangle]
pub extern "C" fn jank_abs(x: i64) -> i64 {
    x.abs()
}

/// Power function
#[no_mangle]
pub extern "C" fn jank_pow(base: i64, exp: i64) -> i64 {
    if exp < 0 {
        0 // Integer power with negative exp → 0
    } else {
        base.pow(exp as u32)
    }
}

/// Modulo (remainder)
#[no_mangle]
pub extern "C" fn jank_mod(a: i64, b: i64) -> i64 {
    if b == 0 {
        0 // Avoid division by zero
    } else {
        a % b
    }
}

/// Quotient (integer division)
#[no_mangle]
pub extern "C" fn jank_quot(a: i64, b: i64) -> i64 {
    if b == 0 {
        0 // Avoid division by zero
    } else {
        a / b
    }
}

/// Minimum of two values
#[no_mangle]
pub extern "C" fn jank_min(a: i64, b: i64) -> i64 {
    a.min(b)
}

/// Maximum of two values
#[no_mangle]
pub extern "C" fn jank_max(a: i64, b: i64) -> i64 {
    a.max(b)
}

/// Random integer (0 to n-1)
#[no_mangle]
pub extern "C" fn jank_rand_int(n: i64) -> i64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    if n <= 0 {
        return 0;
    }
    // Simple PRNG based on time (not cryptographically secure)
    let seed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() as i64;
    (seed.abs() % n).abs()
}

// ============================================================================
// Bit Operations
// ============================================================================

/// Bitwise AND
#[no_mangle]
pub extern "C" fn jank_bit_and(a: i64, b: i64) -> i64 {
    a & b
}

/// Bitwise OR
#[no_mangle]
pub extern "C" fn jank_bit_or(a: i64, b: i64) -> i64 {
    a | b
}

/// Bitwise XOR
#[no_mangle]
pub extern "C" fn jank_bit_xor(a: i64, b: i64) -> i64 {
    a ^ b
}

/// Bitwise NOT
#[no_mangle]
pub extern "C" fn jank_bit_not(a: i64) -> i64 {
    !a
}

/// Left shift
#[no_mangle]
pub extern "C" fn jank_bit_shift_left(a: i64, n: i64) -> i64 {
    if n < 0 || n >= 64 {
        0
    } else {
        a << n
    }
}

/// Right shift (arithmetic)
#[no_mangle]
pub extern "C" fn jank_bit_shift_right(a: i64, n: i64) -> i64 {
    if n < 0 || n >= 64 {
        if a < 0 { -1 } else { 0 }
    } else {
        a >> n
    }
}

// ============================================================================
// I/O Functions (Tagged values)
// ============================================================================

/// Print a tagged value followed by newline
#[no_mangle]
pub extern "C" fn jank_println(val: u64) -> u64 {
    let tagged = Tagged::from_bits(val);
    println!("{:?}", tagged);
    NIL
}

/// Print a tagged value without newline
#[no_mangle]
pub extern "C" fn jank_print(val: u64) -> u64 {
    let tagged = Tagged::from_bits(val);
    print!("{:?}", tagged);
    NIL
}

/// Print an integer (fast path)
#[no_mangle]
pub extern "C" fn jank_println_int(val: i64) -> i64 {
    println!("{}", val);
    0 // Return 0 (nil equivalent for i64)
}

// ============================================================================
// TAGGED VALUE FUNCTIONS (u64 - NaN-boxed)
// These work with mixed types via NaN-boxing. The JIT calls these for ALL ops.
// ============================================================================

/// Add two tagged values (handles integers and floats)
#[no_mangle]
pub extern "C" fn jank_tagged_add(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    // Fast path for integers
    if tagged_a.is_integer() && tagged_b.is_integer() {
        let result = tagged_a.as_integer_unchecked() + tagged_b.as_integer_unchecked();
        return Tagged::integer(result).to_bits();
    }

    // Float path
    let fa = if tagged_a.is_integer() {
        tagged_a.as_integer_unchecked() as f64
    } else if tagged_a.is_float() {
        tagged_a.as_float()
    } else {
        0.0
    };
    let fb = if tagged_b.is_integer() {
        tagged_b.as_integer_unchecked() as f64
    } else if tagged_b.is_float() {
        tagged_b.as_float()
    } else {
        0.0
    };

    Tagged::float(fa + fb).to_bits()
}

/// Subtract two tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_sub(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        let result = tagged_a.as_integer_unchecked() - tagged_b.as_integer_unchecked();
        return Tagged::integer(result).to_bits();
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    Tagged::float(fa - fb).to_bits()
}

/// Multiply two tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_mul(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        let result = tagged_a.as_integer_unchecked() * tagged_b.as_integer_unchecked();
        return Tagged::integer(result).to_bits();
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    Tagged::float(fa * fb).to_bits()
}

/// Divide two tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_div(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    if fb == 0.0 {
        return NIL; // Division by zero returns nil
    }

    let result = fa / fb;
    if result.fract() == 0.0 && result.abs() < i64::MAX as f64 {
        Tagged::integer(result as i64).to_bits()
    } else {
        Tagged::float(result).to_bits()
    }
}

/// Increment a tagged value
#[no_mangle]
pub extern "C" fn jank_tagged_inc(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);
    if tagged.is_integer() {
        Tagged::integer(tagged.as_integer_unchecked() + 1).to_bits()
    } else if tagged.is_float() {
        Tagged::float(tagged.as_float() + 1.0).to_bits()
    } else {
        NIL
    }
}

/// Decrement a tagged value
#[no_mangle]
pub extern "C" fn jank_tagged_dec(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);
    if tagged.is_integer() {
        Tagged::integer(tagged.as_integer_unchecked() - 1).to_bits()
    } else if tagged.is_float() {
        Tagged::float(tagged.as_float() - 1.0).to_bits()
    } else {
        NIL
    }
}

/// Equality check for tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_eq(a: u64, b: u64) -> u64 {
    // Fast path: bit equality for tagged values
    if a == b {
        return TRUE;
    }

    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    // Integer/float comparison
    if (tagged_a.is_integer() || tagged_a.is_float()) &&
       (tagged_b.is_integer() || tagged_b.is_float()) {
        let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
        let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };
        return if fa == fb { TRUE } else { FALSE };
    }

    FALSE
}

/// Less-than comparison for tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_lt(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        return if tagged_a.as_integer_unchecked() < tagged_b.as_integer_unchecked() { TRUE } else { FALSE };
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    if fa < fb { TRUE } else { FALSE }
}

/// Greater-than comparison for tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_gt(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        return if tagged_a.as_integer_unchecked() > tagged_b.as_integer_unchecked() { TRUE } else { FALSE };
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    if fa > fb { TRUE } else { FALSE }
}

/// Less-than-or-equal comparison for tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_lte(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        return if tagged_a.as_integer_unchecked() <= tagged_b.as_integer_unchecked() { TRUE } else { FALSE };
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    if fa <= fb { TRUE } else { FALSE }
}

/// Greater-than-or-equal comparison for tagged values
#[no_mangle]
pub extern "C" fn jank_tagged_gte(a: u64, b: u64) -> u64 {
    let tagged_a = Tagged::from_bits(a);
    let tagged_b = Tagged::from_bits(b);

    if tagged_a.is_integer() && tagged_b.is_integer() {
        return if tagged_a.as_integer_unchecked() >= tagged_b.as_integer_unchecked() { TRUE } else { FALSE };
    }

    let fa = if tagged_a.is_integer() { tagged_a.as_integer_unchecked() as f64 } else { tagged_a.as_float() };
    let fb = if tagged_b.is_integer() { tagged_b.as_integer_unchecked() as f64 } else { tagged_b.as_float() };

    if fa >= fb { TRUE } else { FALSE }
}

/// Check if tagged value is zero
#[no_mangle]
pub extern "C" fn jank_tagged_zero_p(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);
    if tagged.is_integer() {
        if tagged.as_integer_unchecked() == 0 { TRUE } else { FALSE }
    } else if tagged.is_float() {
        if tagged.as_float() == 0.0 { TRUE } else { FALSE }
    } else {
        FALSE
    }
}

/// Check if tagged value is positive
#[no_mangle]
pub extern "C" fn jank_tagged_pos_p(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);
    if tagged.is_integer() {
        if tagged.as_integer_unchecked() > 0 { TRUE } else { FALSE }
    } else if tagged.is_float() {
        if tagged.as_float() > 0.0 { TRUE } else { FALSE }
    } else {
        FALSE
    }
}

/// Check if tagged value is negative
#[no_mangle]
pub extern "C" fn jank_tagged_neg_p(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);
    if tagged.is_integer() {
        if tagged.as_integer_unchecked() < 0 { TRUE } else { FALSE }
    } else if tagged.is_float() {
        if tagged.as_float() < 0.0 { TRUE } else { FALSE }
    } else {
        FALSE
    }
}

/// Check if tagged value is nil
#[no_mangle]
pub extern "C" fn jank_tagged_nil_p(a: u64) -> u64 {
    if a == NIL { TRUE } else { FALSE }
}

/// Logical not for tagged value
#[no_mangle]
pub extern "C" fn jank_tagged_not(a: u64) -> u64 {
    // Only nil and false are falsy
    if a == NIL || a == FALSE {
        TRUE
    } else {
        FALSE
    }
}

/// Check if tagged value is truthy
#[no_mangle]
pub extern "C" fn jank_tagged_is_truthy(a: u64) -> u64 {
    if a == NIL || a == FALSE {
        FALSE
    } else {
        TRUE
    }
}

/// Print tagged value with newline
#[no_mangle]
pub extern "C" fn jank_tagged_println(a: u64) -> u64 {
    let tagged = Tagged::from_bits(a);

    if tagged.is_nil() {
        println!("nil");
    } else if tagged.is_true() {
        println!("true");
    } else if tagged.is_false() {
        println!("false");
    } else if tagged.is_integer() {
        println!("{}", tagged.as_integer_unchecked());
    } else if tagged.is_float() {
        println!("{}", tagged.as_float());
    } else {
        println!("<object>");
    }

    NIL
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_sqrt() {
        assert_eq!(jank_sqrt(0), 0);
        assert_eq!(jank_sqrt(1), 1);
        assert_eq!(jank_sqrt(4), 2);
        assert_eq!(jank_sqrt(16), 4);
        assert_eq!(jank_sqrt(100), 10);
        assert_eq!(jank_sqrt(-1), 0); // Negative → 0
    }

    #[test]
    fn test_abs() {
        assert_eq!(jank_abs(5), 5);
        assert_eq!(jank_abs(-5), 5);
        assert_eq!(jank_abs(0), 0);
    }

    #[test]
    fn test_pow() {
        assert_eq!(jank_pow(2, 0), 1);
        assert_eq!(jank_pow(2, 1), 2);
        assert_eq!(jank_pow(2, 10), 1024);
        assert_eq!(jank_pow(3, 3), 27);
        assert_eq!(jank_pow(2, -1), 0); // Negative exp → 0
    }

    #[test]
    fn test_mod_quot() {
        assert_eq!(jank_mod(10, 3), 1);
        assert_eq!(jank_mod(10, 5), 0);
        assert_eq!(jank_quot(10, 3), 3);
        assert_eq!(jank_quot(10, 5), 2);
    }

    #[test]
    fn test_min_max() {
        assert_eq!(jank_min(3, 5), 3);
        assert_eq!(jank_min(5, 3), 3);
        assert_eq!(jank_max(3, 5), 5);
        assert_eq!(jank_max(5, 3), 5);
    }

    #[test]
    fn test_bit_ops() {
        assert_eq!(jank_bit_and(0b1010, 0b1100), 0b1000);
        assert_eq!(jank_bit_or(0b1010, 0b1100), 0b1110);
        assert_eq!(jank_bit_xor(0b1010, 0b1100), 0b0110);
        assert_eq!(jank_bit_shift_left(1, 4), 16);
        assert_eq!(jank_bit_shift_right(16, 2), 4);
    }

    #[test]
    fn test_println_int() {
        // Just verify it doesn't crash
        assert_eq!(jank_println_int(42), 0);
    }
}
