#include "../skyrim-platform/src/platform_se/skyrim_platform/NativeInventoryMetadata.h"
#include <catch2/catch_all.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace {
// Stack-owned extra records stand in for engine allocation only. The encoder
// under test is the same template that InventoryApi.cpp instantiates with
// CommonLib records and writes directly to its N-API object.
struct Form
{
  uint32_t formID;
};
struct EnchantmentExtra
{
  Form* enchantment;
  uint16_t charge;
  bool removeOnUnequip;
};
enum class SoulLevel
{
  None = 0,
  Petty,
  Lesser,
  Common,
  Greater,
  Grand
};
struct SoulExtra
{
  SoulLevel soul;
  int GetType() const { return 156; }
  SoulLevel GetContainedSoul() const { return soul; }
};
struct ChargeExtra
{
  float charge;
};

using Field = std::variant<double, bool, std::string>;
using Fields = std::map<std::string, Field>;
struct Collect
{
  Fields& fields;
  void operator()(const char* name, double value)
  {
    fields.emplace(name, value);
  }
  void operator()(const char* name, bool value)
  {
    fields.emplace(name, value);
  }
  void operator()(const char* name, const char* value)
  {
    fields.emplace(name, std::string(value));
  }
};
}

TEST_CASE("Native enchantment metadata exports the complete declared contract",
          "[NativeInventoryMetadata]")
{
  Form form{ 0xff001234 };
  for (bool removeOnUnequip : { false, true }) {
    Fields fields;
    NativeInventoryMetadata::Enchantment(
      EnchantmentExtra{ &form, 65535, removeOnUnequip }, Collect{ fields });
    const Fields expected{ { "type", std::string("Enchantment") },
                           { "enchantmentId",
                             static_cast<double>(form.formID) },
                           { "maxCharge", 65535.0 },
                           { "removeOnUnequip", removeOnUnequip } };
    REQUIRE(fields == expected);
  }
  Fields missingForm;
  NativeInventoryMetadata::Enchantment(EnchantmentExtra{ nullptr, 0, false },
                                       Collect{ missingForm });
  REQUIRE(missingForm ==
          Fields{ { "type", std::string("Enchantment") },
                  { "enchantmentId", 0.0 },
                  { "maxCharge", 0.0 },
                  { "removeOnUnequip", false } });
}

TEST_CASE("Native soul metadata exports contained level instead of record tag",
          "[NativeInventoryMetadata]")
{
  for (int soul = 0; soul <= 5; ++soul) {
    SoulExtra extra{ static_cast<SoulLevel>(soul) };
    Fields fields;
    NativeInventoryMetadata::Soul(extra, Collect{ fields });
    REQUIRE(extra.GetType() == 156);
    REQUIRE(fields ==
            Fields{ { "type", std::string("Soul") },
                    { "soul", static_cast<double>(soul) } });
  }
}

TEST_CASE(
  "Native charge metadata retains explicit zero and raw float32 charge",
  "[NativeInventoryMetadata]")
{
  Fields absent;
  NativeInventoryMetadata::Charge<ChargeExtra>(nullptr, Collect{ absent });
  REQUIRE(absent.empty());
  for (float charge : { 0.0f, 0.1f, 37.5f, 100.0f }) {
    Fields fields;
    ChargeExtra extra{ charge };
    NativeInventoryMetadata::Charge(&extra, Collect{ fields });
    REQUIRE(fields ==
            Fields{ { "type", std::string("Charge") },
                    { "charge", static_cast<double>(charge) } });
    REQUIRE(fields.count("charge") == 1);
  }
}
