#include "archives/BitStreamInputArchive.h"
#include "archives/BitStreamOutputArchive.h"
#include <catch2/catch_all.hpp>
#include <limits>

namespace {
struct BinaryFixtureRecord
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("marker", marker)
      .Serialize("enabled", enabled)
      .Serialize("id", id)
      .Serialize("values", values)
      .Serialize("name", name)
      .Serialize("optional", optional);
  }

  std::integral_constant<uint8_t, 37> marker;
  bool enabled = true;
  uint32_t id = 0x12345678;
  std::vector<float> values{ 1.5f, -2.25f, 3 };
  std::string name = "test";
  std::optional<std::array<uint32_t, 2>> optional{
    std::array<uint32_t, 2>{ 13, 21 }
  };
};
}

TEST_CASE("Binary input rejects every truncated compound record",
          "[Archives][Serialization][BinaryInputValidation]")
{
  BinaryFixtureRecord expected;
  SLNet::BitStream encoded;
  BitStreamOutputArchive output(encoded);
  expected.Serialize(output);
  for (size_t length = 0; length < encoded.GetNumberOfBytesUsed(); ++length) {
    CAPTURE(length);
    SLNet::BitStream stream(encoded.GetData(), length, false);
    BitStreamInputArchive input(stream);
    BinaryFixtureRecord actual;
    REQUIRE_THROWS(actual.Serialize(input));
  }
  SLNet::BitStream stream(encoded.GetData(), encoded.GetNumberOfBytesUsed(),
                          false);
  BitStreamInputArchive input(stream);
  BinaryFixtureRecord actual;
  actual.enabled = false;
  actual.id = 0;
  actual.values.clear();
  actual.name.clear();
  actual.optional.reset();
  REQUIRE_NOTHROW(actual.Serialize(input));
  REQUIRE(actual.enabled == expected.enabled);
  REQUIRE(actual.id == expected.id);
  REQUIRE(actual.values == expected.values);
  REQUIRE(actual.name == expected.name);
  REQUIRE(actual.optional == expected.optional);
  SLNet::BitStream reencoded;
  BitStreamOutputArchive roundTrip(reencoded);
  actual.Serialize(roundTrip);
  REQUIRE(reencoded.GetNumberOfBitsUsed() == encoded.GetNumberOfBitsUsed());
  REQUIRE(std::vector<uint8_t>(reencoded.GetData(),
                               reencoded.GetData() +
                                 reencoded.GetNumberOfBytesUsed()) ==
          std::vector<uint8_t>(encoded.GetData(),
                               encoded.GetData() +
                                 encoded.GetNumberOfBytesUsed()));
}

TEST_CASE("Binary counts must fit the remaining input before allocation",
          "[Archives][Serialization][BinaryInputValidation]")
{
  SLNet::BitStream stream;
  stream.Write(std::numeric_limits<uint32_t>::max());
  stream.Write(uint8_t{ 0 });
  BitStreamInputArchive input(stream);
  SECTION("Archive string")
  {
    std::string value;
    REQUIRE_THROWS(input.Serialize("value", value));
    REQUIRE(value.empty());
  }
  SECTION("Archive arithmetic container")
  {
    std::vector<uint32_t> value;
    REQUIRE_THROWS(input.Serialize("value", value));
    REQUIRE(value.empty());
  }
  SECTION("Archive compound container")
  {
    std::vector<std::string> value;
    REQUIRE_THROWS(input.Serialize("value", value));
    REQUIRE(value.empty());
  }
  SECTION("Direct string helper preserves the old value on failure")
  {
    std::string value = "unchanged";
    REQUIRE_THROWS(SerializationUtil::ReadFromBitStream(stream, value));
    REQUIRE(value == "unchanged");
  }
}

TEST_CASE("Direct binary string and array helpers reject short input",
          "[Archives][Serialization][BinaryInputValidation]")
{
  SLNet::BitStream stream;
  SECTION("Missing string length")
  {
    std::string value = "unchanged";
    REQUIRE_THROWS(SerializationUtil::ReadFromBitStream(stream, value));
    REQUIRE(value == "unchanged");
  }
  SECTION("Short string contents")
  {
    stream.Write(uint32_t{ 3 });
    stream.Write('a');
    std::string value = "unchanged";
    REQUIRE_THROWS(SerializationUtil::ReadFromBitStream(stream, value));
    REQUIRE(value == "unchanged");
  }
  SECTION("Short array")
  {
    stream.Write(uint32_t{ 7 });
    std::array<uint32_t, 2> value{};
    REQUIRE_THROWS(SerializationUtil::ReadFromBitStream(stream, value));
  }
  SECTION("Valid empty string")
  {
    stream.Write(uint32_t{ 0 });
    std::string value = "previous";
    REQUIRE_NOTHROW(SerializationUtil::ReadFromBitStream(stream, value));
    REQUIRE(value.empty());
  }
}

TEST_CASE("Binary input validates constant wire markers",
          "[Archives][Serialization][BinaryInputValidation]")
{
  SLNet::BitStream stream;
  stream.Write(uint8_t{ 36 });
  BitStreamInputArchive input(stream);
  std::integral_constant<uint8_t, 37> marker;
  REQUIRE_THROWS(input.Serialize("marker", marker));
}
