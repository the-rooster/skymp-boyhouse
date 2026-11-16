#include "CampaignReferencesBinding.h"
#include "CampaignReferenceFactory.h"
#include "NapiHelper.h"

void CampaignReferencesBinding::Set(Napi::Env env, ScampServer& server,
                                    uint32_t id, Napi::Value command)
{
  const auto value =
    nlohmann::json::parse(NapiHelper::Stringify(env, command));
  if (!value.is_object() || value.size() != 2 ||
      value.contains("references") == value.contains("area"))
    throw std::invalid_argument("Invalid campaign allocation command");
  const auto& level = value.at("encounterLevel");
  if (!level.is_number_integer() || level.get<int64_t>() < 1 ||
      level.get<uint64_t>() > 65535)
    throw std::invalid_argument("Invalid encounter level");
  auto& owner = server.GetPartOne()->worldState.GetFormAt<MpActor>(id);
  if (value.contains("area")) {
    const auto& area = value.at("area");
    if (!area.is_object() || area.size() != 3 ||
        !area.at("worldOrCell").is_number_integer() ||
        area.at("worldOrCell").get<int64_t>() <= 0 ||
        area.at("worldOrCell").get<uint64_t>() >= 0xff000000 ||
        !area.at("center").is_array() || area.at("center").size() != 3 ||
        !area.at("radius").is_number())
      throw std::invalid_argument("Invalid campaign allocation area");
    for (const auto& coordinate : area.at("center"))
      if (!coordinate.is_number())
        throw std::invalid_argument("Invalid campaign allocation center");
    CampaignReferenceFactory::EnsureArea(
      owner, area.at("worldOrCell").get<uint32_t>(),
      { area["center"][0].get<float>(), area["center"][1].get<float>(),
        area["center"][2].get<float>() },
      area.at("radius").get<float>(), level.get<uint32_t>());
    return;
  }
  if (!value.at("references").is_array() ||
      value.at("references").size() > 4096)
    throw std::invalid_argument("Invalid campaign allocation references");
  std::vector<uint32_t> originals;
  for (const auto& ref : value.at("references")) {
    if (!ref.is_number_integer() || ref.get<int64_t>() <= 0 ||
        ref.get<uint64_t>() >= 0xff000000)
      throw std::invalid_argument("Invalid campaign original reference");
    originals.push_back(ref.get<uint32_t>());
  }
  CampaignReferenceFactory::Ensure(owner, originals, level.get<uint32_t>());
}
