#include "CampaignFactionApi.h"
#include "CampaignReferenceBinding.h"
#include "EvaluateTemplate.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>

namespace {
std::vector<Faction> BaseFactions(WorldState& world, uint32_t baseId,
                                  const std::vector<FormDesc>& templateChain)
{
  auto result = EvaluateTemplate<espm::NPC_::UseFactions>(
    &world, baseId, templateChain, [&](const auto& record, const auto& npc) {
      if (record.rec->IsDeleted())
        throw std::runtime_error("Campaign faction template is deleted");
      std::vector<Faction> factions;
      if (npc.factions.size() > Faction::kMaxEntries)
        throw std::runtime_error("Campaign base faction limit exceeded");
      factions.reserve(npc.factions.size());
      for (const auto& faction : npc.factions)
        factions.push_back(
          { FormDesc::FromFormId(record.ToGlobalId(faction.formId),
                                 world.espmFiles),
            faction.rank });
      return factions;
    });
  world.ValidateCampaignFactions(result);
  return result;
}

bool PlayerExpelled(WorldState& world, uint32_t factionId)
{
  const auto record = world.GetEspm().GetBrowser().LookupById(factionId);
  if (!factionId || !record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "FACT")
    throw std::invalid_argument("Expected a live FACT record");
  std::optional<uint32_t> flags;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (std::memcmp(tag, "DATA", 4))
        return;
      if (flags || size != sizeof(uint32_t))
        throw std::runtime_error("Invalid campaign faction flags");
      uint32_t value;
      std::memcpy(&value, data, sizeof(value));
      flags = value;
    },
    world.GetEspmCache());
  if (!flags)
    throw std::runtime_error("Campaign faction has no flags");
  return (*flags & 0x4u) != 0; // CommonLib FACTION_DATA::kPlayerIsExpelled.
}

int Rank(const VarValue& value)
{
  if (value.GetType() != VarValue::kType_Integer)
    throw std::invalid_argument("Campaign faction rank must be an integer");
  const auto rank = static_cast<int32_t>(value);
  if (rank < -128 || rank > 127)
    throw std::invalid_argument("Campaign faction rank is outside [-128,127]");
  // Vanilla unique actors mutate their base for these ranks; an isolated
  // override can instead retain base membership. Full signed reads remain
  // supported, but ambiguous writes require explicit base-layer modeling.
  if (rank < -1)
    throw std::runtime_error(
      "Campaign faction writes below -1 require base-layer semantics");
  return rank;
}

void SetRank(MpActor& actor, MpObjectReference& target, uint32_t factionId,
             int rank)
{
  CampaignFactionApi::ValidateFaction(actor, factionId);
  const auto descriptor =
    FormDesc::FromFormId(factionId, actor.GetParent()->espmFiles);
  if (rank == -1) {
    const auto& state = QuestJournal::ReadReference(actor, target);
    const bool unique = EvaluateTemplate<espm::NPC_::UseBaseData>(
      actor.GetParent(), target.GetBaseId(), state.templateChain,
      [](const auto& record, const auto& npc) {
        if (record.rec->IsDeleted())
          throw std::runtime_error("Campaign faction template is deleted");
        return npc.isUnique;
      });
    const auto& current = QuestJournal::ReadFactions(actor, target);
    if (unique &&
        std::any_of(current.begin(), current.end(), [&](const auto& faction) {
          return faction.formDesc == descriptor;
        }))
      // Vanilla unique actors erase existing base entries at -1, yielding
      // absent rank -2. A reference-local tombstone would report -1 instead.
      // This needs isolated base-layer modeling, not an approximate removal.
      throw std::runtime_error("Removing an existing unique-actor faction "
                               "requires base-layer semantics");
  }
  QuestJournal::EditFactions(actor, target, [&](auto& factions) {
    const auto found =
      std::find_if(factions.begin(), factions.end(), [&](const auto& faction) {
        return faction.formDesc == descriptor;
      });
    if (found == factions.end())
      factions.push_back({ descriptor, static_cast<int8_t>(rank) });
    else
      found->rank = static_cast<int8_t>(rank);
  });
}
}

void CampaignFactionApi::ValidateFaction(MpActor& actor, uint32_t factionId)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign faction context");
  const auto record =
    actor.GetParent()->GetEspm().GetBrowser().LookupById(factionId);
  if (!factionId || !record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "FACT")
    throw std::invalid_argument("Expected a live FACT record");
}

int CampaignFactionApi::GetRank(MpActor& actor, MpObjectReference& target,
                                uint32_t factionId)
{
  ValidateFaction(actor, factionId);
  const auto descriptor =
    FormDesc::FromFormId(factionId, actor.GetParent()->espmFiles);
  const auto& factions = QuestJournal::ReadFactions(actor, target);
  const auto found =
    std::find_if(factions.begin(), factions.end(), [&](const auto& faction) {
      return faction.formDesc == descriptor;
    });
  return found == factions.end() ? -2 : int(found->rank);
}

std::vector<Faction> CampaignFactionApi::ReadBaseFactions(
  MpActor& actor, MpObjectReference& target)
{
  const auto& state = QuestJournal::ReadReference(actor, target);
  if (!target.AsActor())
    throw std::invalid_argument("Campaign faction target must be an actor");
  auto& world = *actor.GetParent();
  return BaseFactions(world, target.GetBaseId(), state.templateChain);
}

bool CampaignFactionApi::IsPlayerExpelled(MpActor& actor, uint32_t factionId)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign faction context");
  return PlayerExpelled(*actor.GetParent(), factionId);
}

std::vector<Faction> CampaignFactionApi::ReadBaseFactions(
  const MpActor& target)
{
  if (!target.GetParent())
    throw std::runtime_error("Unregistered campaign faction context");
  return BaseFactions(*target.GetParent(), target.GetBaseId(),
                      target.GetChangeForm().templateChain);
}

std::vector<Faction> CampaignFactionApi::InitialFactions(const MpActor& target,
                                                         bool isPlayer)
{
  return InitialFactions(target, target.GetChangeForm(), isPlayer);
}

std::vector<Faction> CampaignFactionApi::InitialFactions(
  const MpActor& target, const MpChangeFormREFR& snapshot, bool isPlayer)
{
  auto* world = target.GetParent();
  if (!world)
    throw std::runtime_error("Unregistered campaign faction context");
  if (snapshot.factions) {
    world->ValidateCampaignFactions(*snapshot.factions);
    return *snapshot.factions;
  }
  auto result =
    BaseFactions(*world, target.GetBaseId(), snapshot.templateChain);
  if (isPlayer)
    for (auto& faction : result)
      if (PlayerExpelled(*world, faction.formDesc.ToFormId(world->espmFiles)))
        // Core rank skips expelled base entries for PlayerCharacter, whereas
        // membership still consults the active base. Keep this -2 entry so
        // persisting a different faction does not erase that distinction.
        faction.rank = -2;
  return result;
}

bool CampaignFactionApi::IsMember(MpActor& actor, MpObjectReference& target,
                                  uint32_t factionId)
{
  ValidateFaction(actor, factionId);
  const auto descriptor =
    FormDesc::FromFormId(factionId, actor.GetParent()->espmFiles);
  const auto& factions = QuestJournal::ReadFactions(actor, target);
  const auto base = ReadBaseFactions(actor, target);
  return IsMember(factions, base, descriptor);
}

bool CampaignFactionApi::IsMember(const std::vector<Faction>& effective,
                                  const std::vector<Faction>& base,
                                  const FormDesc& descriptor)
{
  const auto found =
    std::find_if(effective.begin(), effective.end(), [&](const auto& faction) {
      return faction.formDesc == descriptor;
    });
  const bool baseMember =
    std::any_of(base.begin(), base.end(), [&](const auto& faction) {
      return faction.formDesc == descriptor && faction.rank >= 0;
    });
  return IsMember(found == effective.end()
                    ? std::nullopt
                    : std::optional<int8_t>(found->rank),
                  baseMember);
}

bool CampaignFactionApi::IsMember(std::optional<int8_t> effectiveRank,
                                  bool baseMember)
{
  // Native membership preserves an active base under negative overrides other
  // than the explicit -1 tombstone. Expulsion flags do not affect this query.
  return effectiveRank && *effectiveRank != -1 &&
    (*effectiveRank >= 0 || baseMember);
}

void CampaignFactionApi::Register(VirtualMachine& vm, MpActor& actor,
                                  FormId formId)
{
  for (const bool membership : { false, true })
    vm.RegisterFunction(
      "Actor", membership ? "IsInFaction" : "GetFactionRank",
      FunctionType::Method,
      [&, formId, membership](VarValue self, std::vector<VarValue> args) {
        if (args.size() != 1)
          throw std::invalid_argument(
            "Campaign faction query requires one faction");
        auto& target = CampaignReferenceBinding::Resolve(actor, formId(self));
        const auto faction = formId(args[0]);
        return membership ? VarValue(IsMember(actor, target, faction))
                          : VarValue(int32_t(GetRank(actor, target, faction)));
      });
  vm.RegisterFunction("Actor", "SetFactionRank", FunctionType::Method,
                      [&, formId](VarValue self, std::vector<VarValue> args) {
                        if (args.size() != 2)
                          throw std::invalid_argument(
                            "SetFactionRank requires faction and rank");
                        const auto rank = Rank(args[1]);
                        auto& target = CampaignReferenceBinding::Resolve(
                          actor, formId(self));
                        SetRank(actor, target, formId(args[0]), rank);
                        return VarValue::None();
                      });
  vm.RegisterFunction(
    "Actor", "AddToFaction", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 1)
        throw std::invalid_argument("AddToFaction requires one faction");
      auto& target = CampaignReferenceBinding::Resolve(actor, formId(self));
      const auto faction = formId(args[0]);
      // Matches vanilla Actor.psc, including its native membership semantics.
      if (!IsMember(actor, target, faction))
        SetRank(actor, target, faction, 0);
      return VarValue::None();
    });
  vm.RegisterFunction(
    "Actor", "RemoveFromFaction", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 1)
        throw std::invalid_argument("RemoveFromFaction requires one faction");
      auto& target = CampaignReferenceBinding::Resolve(actor, formId(self));
      const auto id = formId(args[0]);
      // Nonunique removal retains/adds -1, including previously absent
      // factions. SetRank rejects unique existing entries until their erase
      // semantics can be projected without mutating a shared actor base.
      SetRank(actor, target, id, -1);
      return VarValue::None();
    });
}
