#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Shared by the JS decoder and the Papyrus adapter. No engine or JS runtime
// dependencies: the boundary rules can be exercised without starting Skyrim.
namespace NativeArrayArguments {
// Bridge resource policy, not an engine/SKSE limit. Bound synchronous decoding
// and allocation while supporting ordinary SKSE arrays beyond Papyrus's 128.
constexpr size_t kMaxElements = 65536;

enum class ElementType
{
  Int,
  Float,
  Bool,
  String,
  Object
};

inline uint32_t Length(size_t length)
{
  if (length > kMaxElements) {
    throw std::runtime_error(
      "Papyrus array argument exceeds bridge limit of 65536 elements");
  }
  return static_cast<uint32_t>(length);
}

inline int32_t Int(double value)
{
  if (!std::isfinite(value) || std::trunc(value) != value ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("Papyrus Int array requires signed int32 values");
  }
  return static_cast<int32_t>(value);
}

inline float Float(double value)
{
  if (!std::isfinite(value) ||
      std::abs(value) > std::numeric_limits<float>::max()) {
    throw std::runtime_error(
      "Papyrus Float array requires finite float32 values");
  }
  const auto result = static_cast<float>(value);
  if (value != 0 && result == 0) {
    throw std::runtime_error("Papyrus Float array value underflows float32");
  }
  return result;
}

inline uint64_t PoolIndex(double value)
{
  constexpr double maxSafeInteger = 9007199254740991.0;
  if (!std::isfinite(value) || std::trunc(value) != value || value < 0 ||
      value > maxSafeInteger) {
    throw std::runtime_error(
      "Native object pool index must be a safe unsigned integer");
  }
  return static_cast<uint64_t>(value);
}

template <class T, class Value>
T Element(const Value& value)
{
  const auto* item = std::get_if<T>(&value);
  if (!item) {
    throw std::runtime_error(
      "Papyrus array element does not match its declared type");
  }
  return *item;
}

// Read returns the same scalar/vector variant used by CallNative. A nested
// array, or a primitive of another kind, cannot be silently coerced. Read must
// reject holes/undefined and resolve object handles before returning them.
template <class Value, class Object, class Read>
Value Decode(ElementType type, size_t length, Read read)
{
  Length(length);
  switch (type) {
    case ElementType::Int:
    case ElementType::Float: {
      std::vector<double> result;
      for (size_t i = 0; i < length; ++i) {
        const auto number = Element<double>(read(i));
        result.push_back(type == ElementType::Int
                           ? static_cast<double>(Int(number))
                           : static_cast<double>(Float(number)));
      }
      return result;
    }
    case ElementType::Bool: {
      std::vector<bool> result;
      for (size_t i = 0; i < length; ++i) {
        result.push_back(Element<bool>(read(i)));
      }
      return result;
    }
    case ElementType::String: {
      std::vector<std::string> result;
      for (size_t i = 0; i < length; ++i) {
        auto item = Element<std::string>(read(i));
        if (item.find('\0') != std::string::npos) {
          throw std::runtime_error(
            "Papyrus String array cannot contain embedded NUL");
        }
        result.push_back(std::move(item));
      }
      return result;
    }
    case ElementType::Object: {
      std::vector<Object> result;
      for (size_t i = 0; i < length; ++i) {
        result.push_back(Element<Object>(read(i)));
      }
      return result;
    }
  }
  throw std::runtime_error("Unsupported Papyrus array element type");
}
}
