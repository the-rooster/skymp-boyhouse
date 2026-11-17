#pragma once
#include "Faction.h"
#include "papyrus-vm/VirtualMachine.h"
#include <functional>
#include <optional>
#include <vector>

class MpActor;
class MpObjectReference;
class MpChangeFormREFR;

// Instance-local faction membership/ranks for the isolated campaign VM.
// Raw SKSE GetFactions traverses base and override layers separately; it is
// intentionally not registered against this effective campaign snapshot.
class CampaignFactionApi
{
public:
  using FormId = std::function<uint32_t(const VarValue&)>;
  static void Register(VirtualMachine& vm, MpActor& actor, FormId formId);
  // Metadata-only validation is safe for structurally preflighted conditions.
  static void ValidateFaction(MpActor& actor, uint32_t factionId);
  static int GetRank(MpActor& actor, MpObjectReference& target,
                     uint32_t factionId);
  static bool IsMember(MpActor& actor, MpObjectReference& target,
                       uint32_t factionId);
  static bool IsMember(const std::vector<Faction>& effective,
                       const std::vector<Faction>& base,
                       const FormDesc& faction);
  static bool IsMember(std::optional<int8_t> effectiveRank, bool baseMember);
  static std::vector<Faction> ReadBaseFactions(MpActor& actor,
                                               MpObjectReference& target);
  static std::vector<Faction> ReadBaseFactions(const MpActor& target);
  static bool IsPlayerExpelled(MpActor& actor, uint32_t factionId);
  // Build a complete effective snapshot from immutable metadata without
  // initializing the live actor or requesting a save. Explicit saved [] wins.
  static std::vector<Faction> InitialFactions(const MpActor& target,
                                              bool isPlayer);
  static std::vector<Faction> InitialFactions(const MpActor& target,
                                              const MpChangeFormREFR& snapshot,
                                              bool isPlayer);
};
