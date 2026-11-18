#include "ActorDialogueConditions.h"
#include "ActorDialogueEditorLocation.h"
#include "CampaignFactionApi.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/CELL.h"
#include "libespm/RecordHeaderAccess.h"
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>

namespace {
using State = ActorDialogueConditionResult::State;
using Clause = espm::QuestLifecycleData::Condition;

espm::LookupResult RequireRecord(WorldState& world, uint32_t id,
                                 const char* type)
{
  const auto result = world.GetEspm().GetBrowser().LookupById(id);
  if (!id || id >= 0xff000000 || !result.rec || result.rec->IsDeleted() ||
      !(result.rec->GetType() == type))
    throw std::runtime_error(std::string("expected a winning ") + type);
  return result;
}

bool InLocation(WorldState& world, uint32_t location, uint32_t expected)
{
  RequireRecord(world, expected, "LCTN");
  std::set<uint32_t> seen;
  while (location) {
    if (!seen.insert(location).second)
      throw std::runtime_error("cyclic location hierarchy");
    const auto loc = RequireRecord(world, location, "LCTN");
    if (location == expected)
      return true;
    std::optional<uint32_t> parent;
    espm::RecordHeaderAccess::IterateFields(
      loc.rec,
      [&](const char* tag, uint32_t size, const char* bytes) {
        if (std::memcmp(tag, "PNAM", 4))
          return;
        if (parent || size != sizeof(uint32_t))
          throw std::runtime_error("invalid location parent");
        uint32_t raw;
        std::memcpy(&raw, bytes, sizeof(raw));
        parent = raw ? loc.ToGlobalId(raw) : 0;
      },
      world.GetEspmCache());
    location = parent.value_or(0);
  }
  return false;
}

bool Matches(bool value, const espm::CTDA& data)
{
  using Op = espm::CTDA::Operator;
  const float actual = value ? 1.f : 0.f;
  const float target = data.comparisonValue;
  switch (data.GetOperator()) {
    case Op::EqualTo:
      return actual == target;
    case Op::NotEqualTo:
      return actual != target;
    case Op::GreaterThen:
      return actual > target;
    case Op::GreaterThenOrEqualTo:
      return actual >= target;
    case Op::LessThen:
      return actual < target;
    case Op::LessThenOrEqualTo:
      return actual <= target;
  }
  throw std::runtime_error("unknown comparison operator");
}

bool InCell(MpActor& subject, uint32_t expected)
{
  auto& world = *subject.GetParent();
  const auto current = subject.GetCellOrWorld().ToFormId(world.espmFiles);
  // An exterior WRLD alone cannot identify the actor's parent CELL.
  RequireRecord(world, current, "CELL");
  RequireRecord(world, expected, "CELL");
  return current == expected;
}

bool EvaluateClause(MpActor& actor1, MpActor& actor2, MpActor* campaignOwner,
                    uint32_t location, const espm::LookupResult& record,
                    const Clause& clause)
{
  const auto& data = clause.data;
  if ((data.operatorFlag & 0x1e) || !std::isfinite(data.comparisonValue) ||
      clause.parameter1String || clause.parameter2String)
    throw std::runtime_error(
      "unsupported flags, comparison or string operand");
  uint32_t raw;
  std::memcpy(&raw, data.functionData, sizeof(raw));
  const auto id = raw ? record.ToGlobalId(raw) : 0;
  if (data.functionIndex == 67 &&
      data.runOnType == espm::CTDA::RunOnTypeFlags::Reference &&
      record.ToGlobalId(static_cast<uint32_t>(data.reference)) == 0x14) {
    if (!campaignOwner)
      throw std::runtime_error(
        "GetInCell on PlayerRef requires the campaign owner");
    return Matches(InCell(*campaignOwner, id), data);
  }
  if (data.runOnType != espm::CTDA::RunOnTypeFlags::EventData ||
      (data.unknown2 != 0x3152 && data.unknown2 != 0x3252))
    throw std::runtime_error("requires event member R1 or R2");
  MpActor& subject = data.unknown2 == 0x3152 ? actor1 : actor2;
  auto& world = *subject.GetParent();
  switch (data.functionIndex) {
    case 67: // GetInCell compares the reference's parent CELL by identity.
      return Matches(InCell(subject, id), data);
    case 71: { // GetInFaction on the selected NPC's campaign snapshot
      RequireRecord(world, id, "FACT");
      const auto effective =
        CampaignFactionApi::InitialFactions(subject, false);
      const auto base = CampaignFactionApi::ReadBaseFactions(subject);
      const auto faction = FormDesc::FromFormId(id, world.espmFiles);
      return Matches(CampaignFactionApi::IsMember(effective, base, faction),
                     data);
    }
    case 72: // GetIsID for an NPC base
      RequireRecord(world, id, "NPC_");
      return Matches(subject.GetBaseId() == id, data);
    case 300: { // IsInInterior
      const auto cellId = subject.GetCellOrWorld().ToFormId(world.espmFiles);
      const auto cell = RequireRecord(world, cellId, "CELL");
      const bool interior = espm::Convert<espm::CELL>(cell.rec)
                              ->GetData(world.GetEspmCache())
                              .flags &
        espm::CELL::Interior;
      return Matches(interior, data);
    }
    case 359: // GetInCurrentLoc includes children of the requested location.
      return Matches(InLocation(world, location, id), data);
    case 565: // GetIsEditorLocation uses the original placement, including its
              // parents.
      return Matches(
        InLocation(world, ActorDialogueEditorLocation(subject), id), data);
    default:
      throw std::runtime_error("unsupported story condition function");
  }
}
}

ActorDialogueConditionResult EvaluateActorDialogueConditions(
  MpActor& actor1, MpActor& actor2, uint32_t locationId,
  const espm::LookupResult& record, const std::vector<Clause>& conditions,
  MpActor* campaignOwner)
{
  if (!record.rec || !actor1.GetParent() ||
      actor1.GetParent() != actor2.GetParent() ||
      record.parent != &actor1.GetParent()->GetEspm().GetBrowser())
    return { State::Unsupported, "missing actor-dialogue condition context" };
  if (campaignOwner) {
    const auto& phase = campaignOwner->GetWorldInstance();
    try {
      if (campaignOwner == &actor1 || campaignOwner == &actor2 ||
          campaignOwner->GetParent() != actor1.GetParent() ||
          actor1.GetWorldInstance() != phase ||
          actor2.GetWorldInstance() != phase ||
          QuestJournal::ParseCampaignOwnerId(phase, true) !=
            campaignOwner->GetFormId() ||
          actor1.GetParent()
              ->LookupFormByIdNoLoad(campaignOwner->GetFormId())
              .get() != campaignOwner)
        return { State::Unsupported, "invalid actor-dialogue campaign owner" };
    } catch (const std::runtime_error& e) {
      return { State::Unsupported, e.what() };
    }
  }
  std::string firstFailure, firstUnsupported;
  size_t groupStart = 0;
  bool group = false, groupUnknown = false;
  for (size_t i = 0; i < conditions.size(); ++i) {
    const auto& clause = conditions[i];
    try {
      // Evaluate every clause so an unsupported predicate in an OR group
      // never grants eligibility. These predicates only read world data.
      const bool match = EvaluateClause(actor1, actor2, campaignOwner,
                                        locationId, record, clause);
      group = group || match;
    } catch (const std::runtime_error& e) {
      groupUnknown = true;
      if (firstUnsupported.empty())
        firstUnsupported = "CTDA " +
          std::to_string(clause.data.functionIndex) + " at index " +
          std::to_string(i) + ": " + e.what();
    }
    if (!(clause.data.operatorFlag & 1) || i + 1 == conditions.size()) {
      // A failed AND term rules out the entire list, even when another
      // term is unknown. Every alternative in this OR group must be false.
      if (!group && !groupUnknown && firstFailure.empty()) {
        firstFailure = "CTDA " +
          std::to_string(conditions[groupStart].data.functionIndex) +
          " at index " + std::to_string(groupStart);
        if (i != groupStart)
          firstFailure += " through " + std::to_string(i);
        firstFailure += " evaluated false";
      }
      groupStart = i + 1;
      group = false;
      groupUnknown = false;
    }
  }
  if (!firstFailure.empty())
    return { State::Failed, firstFailure };
  if (!firstUnsupported.empty())
    return { State::Unsupported, firstUnsupported };
  return { State::Passed, {} };
}
