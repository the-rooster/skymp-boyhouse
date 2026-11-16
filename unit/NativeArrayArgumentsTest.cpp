#include "../skyrim-platform/src/platform_se/skyrim_platform/NativeArrayArguments.h"
#include <catch2/catch_all.hpp>
#include <memory>

namespace {
struct Object
{
  int id;
};
struct Undefined
{
};
using ObjectPtr = std::shared_ptr<Object>;
using Value =
  std::variant<ObjectPtr, double, bool, std::string, std::vector<double>,
               std::vector<std::string>, std::vector<bool>,
               std::vector<ObjectPtr>, Undefined>;
using Kind = NativeArrayArguments::ElementType;

Value Decode(Kind kind, const std::vector<Value>& input)
{
  return NativeArrayArguments::Decode<Value, ObjectPtr>(
    kind, input.size(), [&](size_t i) { return input.at(i); });
}
}

TEST_CASE("Native array inputs retain declared empty element kinds",
          "[NativeArrayArguments]")
{
  REQUIRE(std::holds_alternative<std::vector<double>>(Decode(Kind::Int, {})));
  REQUIRE(
    std::holds_alternative<std::vector<double>>(Decode(Kind::Float, {})));
  REQUIRE(std::holds_alternative<std::vector<bool>>(Decode(Kind::Bool, {})));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(
    Decode(Kind::String, {})));
  REQUIRE(
    std::holds_alternative<std::vector<ObjectPtr>>(Decode(Kind::Object, {})));
  // The numeric vectors share storage, but the declared kind determines
  // validation/conversion (and later the actual Papyrus element TypeInfo).
  REQUIRE_THROWS(Decode(Kind::Int, { 1.5 }));
  REQUIRE(std::get<std::vector<double>>(Decode(Kind::Float, { 1.5 }))[0] ==
          1.5);
}

TEST_CASE("Native Int arrays reject coercion and signed overflow",
          "[NativeArrayArguments]")
{
  const auto values = std::get<std::vector<double>>(
    Decode(Kind::Int, { -2147483648.0, -1.0, 0.0, 2147483647.0 }));
  REQUIRE(values ==
          std::vector<double>{ -2147483648.0, -1.0, 0.0, 2147483647.0 });
  for (double invalid : { -2147483649.0, 2147483648.0, 4294967295.0, 0.5, -0.5,
                          std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity() }) {
    REQUIRE_THROWS(Decode(Kind::Int, { invalid }));
  }
  REQUIRE_THROWS(Decode(Kind::Int, { true }));
  REQUIRE_THROWS(Decode(Kind::Int, { std::string("42") }));
  REQUIRE_THROWS(Decode(Kind::Int, { ObjectPtr() }));
}

TEST_CASE("Native Float arrays preserve float32 rounding and reject overflow",
          "[NativeArrayArguments]")
{
  const auto maximum = static_cast<double>(std::numeric_limits<float>::max());
  const auto minimum =
    static_cast<double>(std::numeric_limits<float>::denorm_min());
  const auto values = std::get<std::vector<double>>(
    Decode(Kind::Float, { -maximum, maximum, minimum, -minimum, 0.1, -0.0 }));
  REQUIRE(values[0] == -maximum);
  REQUIRE(values[1] == maximum);
  REQUIRE(values[2] == minimum);
  REQUIRE(values[3] == -minimum);
  REQUIRE(values[4] == static_cast<double>(0.1f));
  REQUIRE(std::signbit(values[5]));
  for (double invalid : { maximum * 2, -maximum * 2, minimum / 4, -minimum / 4,
                          std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity() }) {
    REQUIRE_THROWS(Decode(Kind::Float, { invalid }));
  }
  REQUIRE_THROWS(Decode(Kind::Float, { false }));
}

TEST_CASE("Native bool string and object arrays remain homogeneous",
          "[NativeArrayArguments]")
{
  REQUIRE(std::get<std::vector<bool>>(Decode(Kind::Bool, { true, false })) ==
          std::vector<bool>{ true, false });
  REQUIRE_THROWS(Decode(Kind::Bool, { true, 1.0 }));
  REQUIRE_THROWS(Decode(Kind::Bool, { ObjectPtr() }));
  REQUIRE(std::get<std::vector<std::string>>(
            Decode(Kind::String, { std::string(), std::string("name") })) ==
          std::vector<std::string>{ "", "name" });
  REQUIRE_THROWS(Decode(Kind::String, { std::string("a\0b", 3) }));
  REQUIRE_THROWS(Decode(Kind::String, { std::string("name"), 1.0 }));
  REQUIRE_THROWS(Decode(Kind::String, { ObjectPtr() }));

  auto object = std::make_shared<Object>(Object{ 7 });
  const auto objects = std::get<std::vector<ObjectPtr>>(
    Decode(Kind::Object, { object, ObjectPtr(), object }));
  REQUIRE(objects.size() == 3);
  REQUIRE(objects[0] == object);
  REQUIRE(objects[1] == nullptr);
  REQUIRE(objects[2] == object);
  REQUIRE_THROWS(Decode(Kind::Object, { object, 7.0 }));
}

TEST_CASE("Native arrays reject nested and undefined entries without coercion",
          "[NativeArrayArguments]")
{
  for (auto kind :
       { Kind::Int, Kind::Float, Kind::Bool, Kind::String, Kind::Object }) {
    REQUIRE_THROWS(Decode(kind, { Undefined() }));
    REQUIRE_THROWS(Decode(kind, { std::vector<double>() }));
    REQUIRE_THROWS(Decode(kind, { std::vector<std::string>{ "nested" } }));
  }
}

TEST_CASE("Native array size policy bounds work before decoding entries",
          "[NativeArrayArguments]")
{
  size_t reads = 0;
  auto read = [&](size_t) -> Value {
    ++reads;
    return true;
  };
  auto empty =
    NativeArrayArguments::Decode<Value, ObjectPtr>(Kind::Bool, 0, read);
  REQUIRE(reads == 0);
  REQUIRE(std::get<std::vector<bool>>(empty).empty());
  const auto maximum = NativeArrayArguments::kMaxElements;
  const auto atLimit =
    NativeArrayArguments::Decode<Value, ObjectPtr>(Kind::Bool, maximum, read);
  REQUIRE(reads == maximum);
  REQUIRE(std::get<std::vector<bool>>(atLimit).size() == maximum);
  reads = 0;
  REQUIRE_THROWS((NativeArrayArguments::Decode<Value, ObjectPtr>(
    Kind::Bool, maximum + 1, read)));
  REQUIRE(reads == 0);
  REQUIRE_THROWS(
    NativeArrayArguments::Length(std::numeric_limits<size_t>::max()));
  // The JS reader throws for a missing own index. Decoder must not continue
  // across a hole or reinterpret it as a None object/zero/false.
  auto sparse = [&](size_t) -> Value {
    ++reads;
    throw std::runtime_error("Sparse Papyrus array arguments are unsupported");
  };
  REQUIRE_THROWS((NativeArrayArguments::Decode<Value, ObjectPtr>(
    Kind::Object, maximum, sparse)));
  REQUIRE(reads == 1);
}

TEST_CASE("Native object pool indices cannot coerce into a different handle",
          "[NativeArrayArguments]")
{
  REQUIRE(NativeArrayArguments::PoolIndex(0) == 0);
  REQUIRE(NativeArrayArguments::PoolIndex(34438) == 34438);
  REQUIRE(NativeArrayArguments::PoolIndex(9007199254740991.0) ==
          9007199254740991ULL);
  for (double invalid : { -1.0, 0.5, 9007199254740992.0,
                          std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity() }) {
    REQUIRE_THROWS(NativeArrayArguments::PoolIndex(invalid));
  }
}
