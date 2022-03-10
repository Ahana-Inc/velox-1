/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <folly/dynamic.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include "velox/common/base/Exceptions.h"
#include "velox/type/StringView.h"

namespace facebook::velox {

using int128_t = __int128_t;
static constexpr uint8_t kMaxPrecisionInt128 = 38;
static constexpr uint8_t kDefaultScale = 0;
static constexpr uint8_t kDefaultPrecision = kMaxPrecisionInt128;

/**
 * A wrapper struct over int128_t type.
 * Refer https://gcc.gnu.org/onlinedocs/gcc/Integer-Overflow-Builtins.html
 * for supported arithmetic operations extensions.
 */
struct Int128 {
  int128_t value = 0;
  Int128() = default;
  Int128(const Int128& copy) {
    this->value = copy.value;
  }

  Int128(int128_t value) : value(value) {}

  void operator=(const Int128& rhs) {
    this->value = rhs.value;
  }

  Int128 operator+(Int128& rhs) {
    Int128 sum;
    VELOX_CHECK(!__builtin_add_overflow(this->value, rhs.value, &sum.value));
    return sum;
  }

  Int128 operator*(Int128& rhs) {
    Int128 product;
    VELOX_CHECK(
        !__builtin_mul_overflow(this->value, rhs.value, &product.value));
    return product;
  }

  Int128 operator-(Int128& rhs) {
    Int128 diff;
    VELOX_CHECK(!__builtin_sub_overflow(this->value, rhs.value, &diff.value));
    return diff;
  }

  bool operator==(const Int128& other) const {
    return this->value == other.value;
  }

  Int128 operator~() {
    return ~this->value;
  }
};

/*
 * This class defines the Velox DECIMAL type support to store
 * fixed-point rational numbers.
 */
class Decimal {
 public:
  inline const uint8_t getPrecision() const {
    return precision_;
  }

  inline const uint8_t getScale() const {
    return scale_;
  }

  inline Int128 getUnscaledValue() const {
    return unscaledValue_;
  }

  inline void setUnscaledValue(const Int128& value) {
    unscaledValue_ = value;
  }

  // Needed for serialization of FlatVector<Decimal>
  operator StringView() const {VELOX_NYI()}

  std::string toString() const;

  operator std::string() const {
    return toString();
  }

  bool operator==(const Decimal& other) const {
    return (
        this->unscaledValue_ == other.getUnscaledValue() &&
        this->precision_ == other.getPrecision() &&
        this->scale_ == other.getScale());
  }
  bool operator!=(const Decimal& other) const {
    return true;
  }

  bool operator<(const Decimal& other) const {
    return true;
  }

  bool operator<=(const Decimal& other) const {
    return true;
  }

  bool operator>(const Decimal& other) const {
    return true;
  }

  Decimal(
      Int128 value,
      uint8_t precision = kDefaultPrecision,
      uint8_t scale = kDefaultScale)
      : unscaledValue_(value), precision_(precision), scale_(scale) {}

  constexpr Decimal() = default;

 private:
  Int128 unscaledValue_; // The actual unscaled value with
                         // max precision 38.
  uint8_t precision_ = kDefaultPrecision; // The number of digits in unscaled
                                          // decimal value
  uint8_t scale_ = kDefaultScale; // The number of digits on the right
                                  // of radix point.
};

class DecimalCasts {
 public:
  static Decimal parseStringToDecimal(const std::string& value) {
    // throws overflow exception if length is > 38
    VELOX_CHECK_GT(
        value.length(), 0, "Decimal string must have at least 1 char")
    Int128 unscaledValue;
    uint8_t precision;
    uint8_t scale;
    try {
      parseToInt128(value, unscaledValue, precision, scale);
    } catch (VeloxRuntimeError const& e) {
      VELOX_USER_CHECK(false, "Decimal overflow");
    }
    return Decimal(unscaledValue, precision, scale);
  }

  /**
   */
  static void parseToInt128(
      std::string value,
      Int128& result,
      uint8_t& precision,
      uint8_t& scale) {
    uint8_t pos = 0;
    bool isNegative = false;
    // Remove leading zeroes.
    if (!isdigit(value[pos])) {
      // Presto allows string literals that start with +123.45
      VELOX_USER_CHECK(
          value[pos] == '-' || value[pos] == '+',
          "Illegal decimal value {}",
          value);
      isNegative = value[pos] == '-';
      value = value.erase(0, 1);
    }
    value = value.erase(0, value.find_first_not_of('0'));
    precision = 0;
    scale = 0;
    bool hasScale = false;
    Int128 digit;
    Int128 exponent((int128_t)10);
    while (pos < value.length()) {
      if (value[pos] == '.') {
        hasScale = true;
        pos++;
        continue;
      }
      VELOX_USER_CHECK(std::isdigit(value[pos]), "Invalid decimal string");
      digit.value = value[pos] - '0';
      if (isNegative) {
        result = result * exponent - digit;
      } else {
        result = result * exponent + digit;
      }
      if (hasScale) {
        scale++;
      }
      precision++;
      pos++;
    }
  }
};

void parseTo(folly::StringPiece in, Decimal& out);

template <typename T>
void toAppend(const ::facebook::velox::Decimal& value, T* result) {}
} // namespace facebook::velox

namespace std {
template <>
struct hash<::facebook::velox::Decimal> {
  size_t operator()(const ::facebook::velox::Decimal& value) const {
    return 0;
  }
};

std::string to_string(const ::facebook::velox::Decimal& ts);
} // namespace std

namespace folly {
template <>
struct hasher<::facebook::velox::Decimal> {
  size_t operator()(const ::facebook::velox::Decimal& value) const {
    return 0;
  }
};
} // namespace folly
