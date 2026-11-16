#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>

class MpActor;

// Campaign state shared by Papyrus and the trusted gamemode API. Clients may
// read snapshots but cannot issue mutations. Original stage fragments/alias
// filling must be supplied by a quest adapter before a quest is enabled.
class QuestJournal
{
public:
  static constexpr auto kProperty = "private.boyhouse.questJournal";
  static constexpr auto kEnabled = "private.boyhouse.questAdapters";
  static MpActor& Owner(MpActor& actor);
  static nlohmann::json Snapshot(MpActor& actor);
  static nlohmann::json Get(MpActor& actor, uint32_t questId);
  static bool Apply(MpActor& actor, const nlohmann::json& command);
};
