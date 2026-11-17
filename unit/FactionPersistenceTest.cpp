#include "MpChangeForms.h"
#include <catch2/catch_all.hpp>

namespace {
MpChangeForm Decode(const nlohmann::json& json)
{
  simdjson::dom::parser parser;
  const auto text = json.dump();
  auto document = parser.parse(text).value();
  return MpChangeForm::JsonToChangeForm(document);
}
MpChangeForm WithRank(int rank)
{
  MpChangeForm result;
  result.factions = std::vector<Faction>{ { FormDesc(0x2bf9b, "Skyrim.esm"),
                                            static_cast<int8_t>(rank) } };
  return result;
}
}

TEST_CASE("Faction saves preserve every signed rank and migrate legacy words",
          "[save][CampaignFaction]")
{
  for (int rank = -128; rank <= 127; ++rank) {
    const auto saved = WithRank(rank);
    auto json = MpChangeForm::ToJson(saved);
    REQUIRE(json["factions"]["entries"][0]["rank"] == rank);
    REQUIRE(Decode(json).factions == saved.factions);
    if (rank < 0) {
      json["factions"]["entries"][0]["rank"] =
        static_cast<uint32_t>(static_cast<int8_t>(rank));
      const auto restored = Decode(json);
      REQUIRE(restored.factions == saved.factions);
      REQUIRE(MpChangeForm::ToJson(
                restored)["factions"]["entries"][0]["rank"] == rank);
    }
  }
}

TEST_CASE("Faction save ranks cannot silently wrap or coerce",
          "[save][CampaignFaction]")
{
  auto json = MpChangeForm::ToJson(WithRank(0));
  for (const nlohmann::json& invalid : std::vector<nlohmann::json>{
         -129, 128, 255, 4294967167LL, 4294967296LL, 9223372036854775807LL,
         18446744073709551615ULL, 1.0, -1.5, "1", true, nullptr }) {
    json["factions"]["entries"][0]["rank"] = invalid;
    REQUIRE_THROWS(Decode(json));
  }
  json["factions"]["entries"][0].erase("rank");
  REQUIRE_THROWS(Decode(json));
}

TEST_CASE("Faction snapshots reject duplicate identities and bound allocation",
          "[save][CampaignFaction]")
{
  auto saved = WithRank(1);
  auto json = MpChangeForm::ToJson(saved);
  json["factions"]["entries"].push_back(
    { { "formDesc", "2bf9b:Skyrim.esm" }, { "rank", 2 } });
  REQUIRE_THROWS(Decode(json));
  saved.factions->push_back(saved.factions->front());
  REQUIRE_THROWS(MpChangeForm::ToJson(saved));

  saved.factions->clear();
  for (uint32_t i = 1; i <= Faction::kMaxEntries; ++i)
    saved.factions->push_back({ FormDesc(i, "Skyrim.esm"), 0 });
  json = MpChangeForm::ToJson(saved);
  REQUIRE(Decode(json).factions == saved.factions);
  saved.factions->push_back(
    { FormDesc(Faction::kMaxEntries + 1, "Skyrim.esm"), 0 });
  REQUIRE_THROWS(MpChangeForm::ToJson(saved));
  json["factions"]["entries"].push_back(
    { { "formDesc", "1001:Skyrim.esm" }, { "rank", 0 } });
  REQUIRE_THROWS(Decode(json));
}

TEST_CASE(
  "Faction initialization and rank changes participate in saved equality",
  "[save][CampaignFaction]")
{
  MpChangeForm absent;
  auto empty = absent;
  empty.factions = std::vector<Faction>{};
  REQUIRE(absent != empty);
  REQUIRE_FALSE(Decode(MpChangeForm::ToJson(absent)).factions.has_value());
  const auto restoredEmpty = Decode(MpChangeForm::ToJson(empty));
  REQUIRE(restoredEmpty.factions.has_value());
  REQUIRE(restoredEmpty.factions->empty());
  const auto first = WithRank(0), second = WithRank(1);
  REQUIRE(first != second);
  REQUIRE(first < second);
  REQUIRE(Decode(MpChangeForm::ToJson(first)) == first);
}
