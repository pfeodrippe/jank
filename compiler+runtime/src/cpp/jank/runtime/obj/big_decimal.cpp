#include <jank/runtime/obj/big_decimal.hpp>
#include <jank/runtime/visit.hpp>
#include <jank/util/fmt.hpp>

namespace jank::runtime
{
// These operator overloads are only needed when native_big_integer is a class type (boost::multiprecision)
// For WASM where native_big_integer is 'long long', these would be invalid (no class type parameter)
#if !defined(JANK_TARGET_EMSCRIPTEN) && !defined(JANK_TARGET_IOS)
  native_big_decimal operator+(native_big_decimal const &l, native_big_integer const &r)
  {
    return l + native_big_decimal(r.str());
  }

  native_big_decimal operator-(native_big_decimal const &l, native_big_integer const &r)
  {
    return l - native_big_decimal(r.str());
  }

  native_big_decimal operator*(native_big_decimal const &l, native_big_integer const &r)
  {
    return l * native_big_decimal(r.str());
  }

  native_big_decimal operator/(native_big_decimal const &l, native_big_integer const &r)
  {
    return l / native_big_decimal(r.str());
  }

  native_big_decimal operator+(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) + r;
  }

  native_big_decimal operator-(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) - r;
  }

  native_big_decimal operator*(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) * r;
  }

  native_big_decimal operator/(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) / r;
  }

  bool operator==(native_big_decimal const &l, native_big_integer const &r)
  {
    native_big_decimal const r_as_decimal(r.str());
    return abs(l - r_as_decimal) < std::numeric_limits<native_big_decimal>::epsilon();
  }

  bool operator!=(native_big_decimal const &l, native_big_integer const &r)
  {
    return !(l == r);
  }

  bool operator==(native_big_integer const &l, native_big_decimal const &r)
  {
    return r == l;
  }

  bool operator!=(native_big_integer const &l, native_big_decimal const &r)
  {
    return !(l == r);
  }

  bool operator>(native_big_decimal const &l, native_big_integer const &r)
  {
    return l > native_big_decimal(r.str());
  }

  bool operator>(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) > r;
  }

  bool operator<(native_big_decimal const &l, native_big_integer const &r)
  {
    return l < native_big_decimal(r.str());
  }

  bool operator<(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) < r;
  }

  bool operator>=(native_big_decimal const &l, native_big_integer const &r)
  {
    return l >= native_big_decimal(r.str());
  }

  bool operator>=(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) >= r;
  }

  bool operator<=(native_big_decimal const &l, native_big_integer const &r)
  {
    return l <= native_big_decimal(r.str());
  }

  bool operator<=(native_big_integer const &l, native_big_decimal const &r)
  {
    return native_big_decimal(l.str()) <= r;
  }
#endif
}

namespace jank::runtime::obj
{
  big_decimal::big_decimal(native_big_decimal const &val)
    : data{ val }
  {
  }

  big_decimal::big_decimal(native_big_decimal &&val)
    : data{ std::move(val) }
  {
  }

  big_decimal::big_decimal(jtl::immutable_string const &val)
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    : data{ std::stod(val.c_str()) }
#else
    : data{ val.c_str() }
#endif
  {
  }

  big_decimal::big_decimal(native_big_integer const &val)
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    : data{ static_cast<native_big_decimal>(val) }
#else
    : data{ val.real() }
#endif
  {
  }

  big_decimal::big_decimal(ratio const &val)
    : data(native_big_decimal(val.data.numerator) / val.data.denominator)
  {
  }

  bool big_decimal::equal(object const &o) const
  {
    return visit_number_like(
      [this](auto const typed_o) -> bool {
        return abs(data - typed_o->data) < std::numeric_limits<f64>::epsilon();
      },
      [&]() -> bool { return false; },
      &o);
  }

  jtl::immutable_string big_decimal::to_string() const
  {
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    return std::to_string(data);
#else
    return data.str();
#endif
  }

  void big_decimal::to_string(jtl::string_builder &buff) const
  {
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    buff(std::to_string(data));
#else
    buff(data.str());
#endif
  }

  jtl::immutable_string big_decimal::to_code_string() const
  {
    return to_string() + 'M';
  }

  uhash big_decimal::to_hash() const
  {
    return std::hash<native_big_decimal>{}(data);
  }

  i64 big_decimal::compare(object const &o) const
  {
    return visit_number_like(
      [this](auto const typed_o) -> i64 { return (data > typed_o->data) - (data < typed_o->data); },
      [&]() -> i64 {
        throw std::runtime_error{ util::format("not comparable: {}", runtime::to_string(&o)) };
      },
      &o);
  }

  i64 big_decimal::to_integer() const
  {
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    return static_cast<i64>(data);
#else
    return data.convert_to<i64>();
#endif
  }

  f64 big_decimal::to_real() const
  {
#if defined(JANK_TARGET_EMSCRIPTEN) || defined(JANK_TARGET_IOS)
    return static_cast<f64>(data);
#else
    return data.convert_to<f64>();
#endif
  }

  object_ref big_decimal::create(jtl::immutable_string const &val)
  {
    return make_box<big_decimal>(val).erase();
  }

}
