//! NaN-boxing implementation for jank-rs
//!
//! This module provides a compact 64-bit tagged value representation using NaN-boxing.
//! This allows the JIT compiler to handle mixed-type code efficiently.
//!
//! ## Encoding Scheme
//!
//! IEEE 754 doubles use 64 bits: 1 sign + 11 exponent + 52 mantissa.
//! NaN is any value where exponent is all 1s (0x7FF) and mantissa is non-zero.
//!
//! We use the quiet NaN space to encode other types:
//! - If (value & QNAN_MASK) != QNAN_MASK: it's a double (stored as-is)
//! - Otherwise: tag bits determine the type
//!
//! Layout for tagged values (when it's not a double):
//! ```text
//! 63       52 51 50 49 48 47                                  0
//! ├──────────┼──┴──┴──┴──┼──────────────────────────────────────┤
//! │ 0x7FFC   │  tag (4)  │          payload (48 bits)           │
//! ```

/// Quiet NaN with sign bit clear - base for all tagged values
/// 0x7FF8_0000_0000_0000 is the canonical qNaN
const QNAN: u64 = 0x7FF8_0000_0000_0000;

/// Mask for checking if a value is a tagged value (not a double)
/// We use bit 49 to distinguish tagged values from doubles
const TAG_BIT: u64 = 0x0004_0000_0000_0000;

/// Combined mask: QNAN + TAG_BIT = 0x7FFC_0000_0000_0000
const TAGGED_MASK: u64 = QNAN | TAG_BIT;

/// Mask for extracting the payload (lower 48 bits)
const PAYLOAD_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;

/// Mask for extracting the tag (bits 48-51)
const TYPE_TAG_MASK: u64 = 0x0003_0000_0000_0000;

/// Type tags (2 bits, stored in bits 48-49)
const TAG_SPECIAL: u64 = 0x0000_0000_0000_0000; // nil, true, false
const TAG_INTEGER: u64 = 0x0001_0000_0000_0000; // 48-bit signed integer
const TAG_POINTER: u64 = 0x0002_0000_0000_0000; // heap pointer

/// Special value payloads (when tag is TAG_SPECIAL)
const SPECIAL_NIL: u64 = 0;
const SPECIAL_FALSE: u64 = 1;
const SPECIAL_TRUE: u64 = 2;

/// Pre-computed tagged values for nil, true, false
pub const NIL: u64 = TAGGED_MASK | TAG_SPECIAL | SPECIAL_NIL;
pub const FALSE: u64 = TAGGED_MASK | TAG_SPECIAL | SPECIAL_FALSE;
pub const TRUE: u64 = TAGGED_MASK | TAG_SPECIAL | SPECIAL_TRUE;

/// A NaN-boxed value that can efficiently represent multiple types in 64 bits.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct Tagged(u64);

impl Tagged {
    /// Create a tagged nil value
    #[inline]
    pub const fn nil() -> Self {
        Tagged(NIL)
    }

    /// Create a tagged boolean
    #[inline]
    pub const fn boolean(b: bool) -> Self {
        Tagged(if b { TRUE } else { FALSE })
    }

    /// Create a tagged integer (48-bit range: -2^47 to 2^47-1)
    #[inline]
    pub fn integer(n: i64) -> Self {
        // We can store 48 bits, sign-extended
        // For now, just truncate to 48 bits
        let payload = (n as u64) & PAYLOAD_MASK;
        Tagged(TAGGED_MASK | TAG_INTEGER | payload)
    }

    /// Create a tagged double (stored as-is if not NaN, or converted)
    #[inline]
    pub fn float(f: f64) -> Self {
        let bits = f.to_bits();
        // If it's a NaN, we need to canonicalize it to avoid collision with tags
        if f.is_nan() {
            // Use the canonical quiet NaN (without our tag bit)
            Tagged(QNAN)
        } else {
            // Store the double bits directly
            Tagged(bits)
        }
    }

    /// Create a tagged pointer to a heap object
    #[inline]
    pub fn pointer(ptr: *const ()) -> Self {
        // Pointers on 64-bit systems typically only use 48 bits
        let payload = (ptr as u64) & PAYLOAD_MASK;
        Tagged(TAGGED_MASK | TAG_POINTER | payload)
    }

    /// Check if this is a double (not a tagged value)
    #[inline]
    pub fn is_double(&self) -> bool {
        // It's a double if the tagged mask bits aren't all set
        (self.0 & TAGGED_MASK) != TAGGED_MASK
    }

    /// Check if this is nil
    #[inline]
    pub fn is_nil(&self) -> bool {
        self.0 == NIL
    }

    /// Check if this is a boolean
    #[inline]
    pub fn is_boolean(&self) -> bool {
        self.0 == TRUE || self.0 == FALSE
    }

    /// Check if this is an integer
    #[inline]
    pub fn is_integer(&self) -> bool {
        (self.0 & TAGGED_MASK) == TAGGED_MASK && (self.0 & TYPE_TAG_MASK) == TAG_INTEGER
    }

    /// Check if this is a pointer
    #[inline]
    pub fn is_pointer(&self) -> bool {
        (self.0 & TAGGED_MASK) == TAGGED_MASK && (self.0 & TYPE_TAG_MASK) == TAG_POINTER
    }

    /// Try to extract as a double
    #[inline]
    pub fn as_double(&self) -> Option<f64> {
        if self.is_double() {
            Some(f64::from_bits(self.0))
        } else {
            None
        }
    }

    /// Try to extract as a boolean
    #[inline]
    pub fn as_boolean(&self) -> Option<bool> {
        if self.0 == TRUE {
            Some(true)
        } else if self.0 == FALSE {
            Some(false)
        } else {
            None
        }
    }

    /// Try to extract as an integer
    #[inline]
    pub fn as_integer(&self) -> Option<i64> {
        if self.is_integer() {
            let payload = self.0 & PAYLOAD_MASK;
            // Sign-extend from 48 bits to 64 bits
            let shifted = (payload as i64) << 16;
            Some(shifted >> 16)
        } else {
            None
        }
    }

    /// Try to extract as a pointer
    #[inline]
    pub fn as_pointer<T>(&self) -> Option<*const T> {
        if self.is_pointer() {
            let payload = self.0 & PAYLOAD_MASK;
            Some(payload as *const T)
        } else {
            None
        }
    }

    /// Get the raw u64 bits
    #[inline]
    pub fn bits(&self) -> u64 {
        self.0
    }

    /// Get the raw u64 bits (alias for bits())
    #[inline]
    pub fn to_bits(&self) -> u64 {
        self.0
    }

    /// Create from raw u64 bits
    #[inline]
    pub fn from_bits(bits: u64) -> Self {
        Tagged(bits)
    }

    /// Check truthiness (Clojure semantics: only nil and false are falsy)
    #[inline]
    pub fn is_truthy(&self) -> bool {
        self.0 != NIL && self.0 != FALSE
    }

    /// Check if this is true
    #[inline]
    pub fn is_true(&self) -> bool {
        self.0 == TRUE
    }

    /// Check if this is false
    #[inline]
    pub fn is_false(&self) -> bool {
        self.0 == FALSE
    }

    /// Check if this is a float/double
    #[inline]
    pub fn is_float(&self) -> bool {
        self.is_double()
    }

    /// Extract as float (returns 0.0 if not a float)
    #[inline]
    pub fn as_float(&self) -> f64 {
        if self.is_double() {
            f64::from_bits(self.0)
        } else {
            0.0
        }
    }

    /// Extract as integer (returns 0 if not an integer)
    /// Use this for JIT code where we know the type
    #[inline]
    pub fn as_integer_unchecked(&self) -> i64 {
        let payload = self.0 & PAYLOAD_MASK;
        let shifted = (payload as i64) << 16;
        shifted >> 16
    }

    /// Extract as pointer (returns null if not a pointer)
    #[inline]
    pub fn as_pointer_unchecked(&self) -> *const () {
        (self.0 & PAYLOAD_MASK) as *const ()
    }
}

impl std::fmt::Debug for Tagged {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_nil() {
            write!(f, "Tagged(nil)")
        } else if let Some(b) = self.as_boolean() {
            write!(f, "Tagged({})", b)
        } else if let Some(n) = self.as_integer() {
            write!(f, "Tagged({})", n)
        } else if let Some(d) = self.as_double() {
            write!(f, "Tagged({:?})", d)
        } else if self.is_pointer() {
            let ptr = self.0 & PAYLOAD_MASK;
            write!(f, "Tagged(ptr: 0x{:x})", ptr)
        } else {
            write!(f, "Tagged(unknown: 0x{:016x})", self.0)
        }
    }
}

impl PartialEq for Tagged {
    fn eq(&self, other: &Self) -> bool {
        if self.is_double() && other.is_double() {
            // For doubles, use f64 equality (handles -0.0 == 0.0 correctly)
            let a = f64::from_bits(self.0);
            let b = f64::from_bits(other.0);
            a == b
        } else {
            // For all other types, bit equality works
            self.0 == other.0
        }
    }
}

impl Eq for Tagged {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_nil() {
        let nil = Tagged::nil();
        assert!(nil.is_nil());
        assert!(!nil.is_boolean());
        assert!(!nil.is_integer());
        assert!(!nil.is_double());
        assert!(!nil.is_truthy());
    }

    #[test]
    fn test_boolean() {
        let t = Tagged::boolean(true);
        let f = Tagged::boolean(false);

        assert!(t.is_boolean());
        assert!(f.is_boolean());
        assert_eq!(t.as_boolean(), Some(true));
        assert_eq!(f.as_boolean(), Some(false));
        assert!(t.is_truthy());
        assert!(!f.is_truthy());
    }

    #[test]
    fn test_integer() {
        let zero = Tagged::integer(0);
        let positive = Tagged::integer(12345);
        let negative = Tagged::integer(-42);
        let large = Tagged::integer(1_000_000_000_000i64);
        let negative_large = Tagged::integer(-1_000_000_000_000i64);

        assert!(zero.is_integer());
        assert!(positive.is_integer());
        assert!(negative.is_integer());

        assert_eq!(zero.as_integer(), Some(0));
        assert_eq!(positive.as_integer(), Some(12345));
        assert_eq!(negative.as_integer(), Some(-42));
        assert_eq!(large.as_integer(), Some(1_000_000_000_000i64));
        assert_eq!(negative_large.as_integer(), Some(-1_000_000_000_000i64));

        // Integers are truthy (even 0)
        assert!(zero.is_truthy());
        assert!(positive.is_truthy());
        assert!(negative.is_truthy());
    }

    #[test]
    fn test_float() {
        let zero = Tagged::float(0.0);
        let pi = Tagged::float(std::f64::consts::PI);
        let negative = Tagged::float(-3.14);
        let large = Tagged::float(1e100);

        assert!(zero.is_double());
        assert!(pi.is_double());
        assert!(negative.is_double());
        assert!(large.is_double());

        assert_eq!(zero.as_double(), Some(0.0));
        assert!((pi.as_double().unwrap() - std::f64::consts::PI).abs() < 1e-15);
        assert_eq!(negative.as_double(), Some(-3.14));
        assert_eq!(large.as_double(), Some(1e100));

        // Floats are truthy
        assert!(zero.is_truthy());
        assert!(pi.is_truthy());
    }

    #[test]
    fn test_nan() {
        let nan = Tagged::float(f64::NAN);
        assert!(nan.is_double());
        assert!(nan.as_double().unwrap().is_nan());
    }

    #[test]
    fn test_infinity() {
        let inf = Tagged::float(f64::INFINITY);
        let neg_inf = Tagged::float(f64::NEG_INFINITY);

        assert!(inf.is_double());
        assert!(neg_inf.is_double());
        assert_eq!(inf.as_double(), Some(f64::INFINITY));
        assert_eq!(neg_inf.as_double(), Some(f64::NEG_INFINITY));
    }

    #[test]
    fn test_pointer() {
        let data: i32 = 42;
        let ptr = &data as *const i32 as *const ();
        let tagged = Tagged::pointer(ptr);

        assert!(tagged.is_pointer());
        assert!(tagged.is_truthy());

        let recovered: *const i32 = tagged.as_pointer().unwrap();
        assert_eq!(unsafe { *recovered }, 42);
    }

    #[test]
    fn test_equality() {
        assert_eq!(Tagged::nil(), Tagged::nil());
        assert_eq!(Tagged::boolean(true), Tagged::boolean(true));
        assert_eq!(Tagged::boolean(false), Tagged::boolean(false));
        assert_ne!(Tagged::boolean(true), Tagged::boolean(false));
        assert_eq!(Tagged::integer(42), Tagged::integer(42));
        assert_ne!(Tagged::integer(42), Tagged::integer(43));
        assert_eq!(Tagged::float(3.14), Tagged::float(3.14));
    }

    #[test]
    fn test_type_discrimination() {
        let nil = Tagged::nil();
        let b = Tagged::boolean(true);
        let i = Tagged::integer(42);
        let f = Tagged::float(3.14);

        // Each type should only match its own predicate
        assert!(nil.is_nil() && !nil.is_boolean() && !nil.is_integer() && !nil.is_double());
        assert!(!b.is_nil() && b.is_boolean() && !b.is_integer() && !b.is_double());
        assert!(!i.is_nil() && !i.is_boolean() && i.is_integer() && !i.is_double());
        assert!(!f.is_nil() && !f.is_boolean() && !f.is_integer() && f.is_double());
    }

    #[test]
    fn test_debug_format() {
        assert_eq!(format!("{:?}", Tagged::nil()), "Tagged(nil)");
        assert_eq!(format!("{:?}", Tagged::boolean(true)), "Tagged(true)");
        assert_eq!(format!("{:?}", Tagged::boolean(false)), "Tagged(false)");
        assert_eq!(format!("{:?}", Tagged::integer(42)), "Tagged(42)");
    }

    #[test]
    fn test_bits_roundtrip() {
        let values = vec![
            Tagged::nil(),
            Tagged::boolean(true),
            Tagged::boolean(false),
            Tagged::integer(42),
            Tagged::integer(-1),
            Tagged::float(3.14),
        ];

        for v in values {
            let bits = v.bits();
            let recovered = Tagged::from_bits(bits);
            assert_eq!(v, recovered);
        }
    }
}
