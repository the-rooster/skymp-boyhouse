#include "CampaignCondition.h"
#include "CampaignFactionApi.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/LIGH.h"
#include "libespm/RecordHeaderAccess.h"
#include "libespm/Utils.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace {
double Finite(double value)
{
  if (!std::isfinite(value))
    throw std::runtime_error("Nonfinite campaign condition value");
  return value;
}

void ValidateItem(MpActor& actor, uint32_t id)
{
  auto* world = actor.GetParent();
  if (!world)
    throw std::runtime_error("Unregistered campaign condition actor");
  const auto record = world->GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      !espm::utils::IsItem(record.rec->GetType()))
    throw std::runtime_error(
      "Campaign item condition requires a concrete base item");
  if (record.rec->GetType() == "LIGH" &&
      !(espm::Convert<espm::LIGH>(record.rec)
          ->GetData(world->GetEspmCache())
          .data.flags &
        espm::LIGH::CanBeCarried))
    throw std::runtime_error(
      "Campaign item condition requires a carryable light");
}

void ValidateReference(MpActor& actor, uint32_t id, bool actorOnly)
{
  // PlayerRef is the campaign owner, not a placed record to materialize.
  if (id == 0x14)
    return;
  auto* world = actor.GetParent();
  if (!world)
    throw std::runtime_error("Unregistered campaign condition actor");
  const auto record = world->GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      (record.rec->GetType() != "ACHR" &&
       (actorOnly || record.rec->GetType() != "REFR")))
    throw std::runtime_error(
      actorOnly ? "Campaign actor condition requires a placed actor reference"
                : "Campaign condition requires a placed reference");
}

espm::LookupResult StaticOperand(MpActor& actor, uint32_t id, const char* type)
{
  auto* world = actor.GetParent();
  if (!world || !id || id >= 0xff000000)
    throw std::runtime_error("Expected a static location condition operand");
  const auto record = world->GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() || record.rec->GetType() != type)
    throw std::runtime_error(std::string("Location condition requires ") +
                             type);
  return record;
}

bool LocationHasKeyword(WorldState& world, const espm::LookupResult& location,
                        uint32_t keyword)
{
  std::optional<uint32_t> count, entries;
  bool found = false;
  espm::RecordHeaderAccess::IterateFields(
    location.rec,
    [&](const char* tag, uint32_t size, const char* bytes) {
      if (!std::memcmp(tag, "KSIZ", 4)) {
        if (count || size != sizeof(uint32_t))
          throw std::runtime_error("Invalid location keyword count");
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        count = value;
      } else if (!std::memcmp(tag, "KWDA", 4)) {
        if (entries || size % sizeof(uint32_t))
          throw std::runtime_error("Invalid location keywords");
        entries = size / sizeof(uint32_t);
        for (uint32_t i = 0; i < *entries; ++i) {
          uint32_t raw;
          std::memcpy(&raw, bytes + i * sizeof(raw), sizeof(raw));
          if (raw && location.ToGlobalId(raw) == keyword)
            found = true;
        }
      }
    },
    world.GetEspmCache());
  if (count.value_or(0) != entries.value_or(0) || (entries && !count))
    throw std::runtime_error("Location keyword count mismatch");
  return found;
}
}

CampaignCondition CampaignCondition::Compile(const espm::CTDA& data,
                                             const MapForm& mapForm)
{
  CampaignCondition result;
  if ((data.operatorFlag & 0x1a) ||
      static_cast<uint8_t>(data.GetOperator()) > 5)
    throw std::runtime_error("Unsupported quest log condition flags");
  switch (data.functionIndex) {
    case 35:
    case 46:
    case 47:
    case 56:
    case 58:
    case 59:
    case 71:
    case 73:
    case 74:
    case 543:
    case 566:
    case 605:
    case 610:
    case 629:
      result.function = static_cast<Function>(data.functionIndex);
      break;
    default:
      throw std::runtime_error("Unsupported quest log condition function");
  }
  const bool physical = result.function == Function::Disabled ||
    result.function == Function::Dead ||
    result.function == Function::ItemCount ||
    result.function == Function::InFaction ||
    result.function == Function::FactionRank ||
    result.function == Function::IsAliasRef;
  const bool locationAlias = result.function == Function::LocAliasIsLocation ||
    result.function == Function::LocAliasHasKeyword;
  if (physical) {
    if (data.runOnType == espm::CTDA::RunOnTypeFlags::Reference) {
      result.selector = Selector::Reference;
      result.referenceOrAlias = mapForm(static_cast<uint32_t>(data.reference));
      if (!result.referenceOrAlias || result.referenceOrAlias >= 0xff000000)
        throw std::runtime_error(
          "Condition requires a logical static reference");
    } else if (data.runOnType == espm::CTDA::RunOnTypeFlags::QuestAlias &&
               (result.function == Function::Dead ||
                result.function == Function::InFaction ||
                result.function == Function::FactionRank ||
                result.function == Function::IsAliasRef) &&
               data.unknown2 >= 0) {
      // TES5 CTDA: reference at byte 24; alias/data ID at byte 28. Alias 0
      // is valid and is never a FormID, even in a master override.
      result.selector = Selector::Alias;
      result.referenceOrAlias = static_cast<uint32_t>(data.unknown2);
    } else {
      throw std::runtime_error(
        "Unsupported physical quest condition run-on context");
    }
  } else if (data.runOnType != espm::CTDA::RunOnTypeFlags::Subject &&
             !(!locationAlias &&
               data.runOnType == espm::CTDA::RunOnTypeFlags::Reference &&
               mapForm(static_cast<uint32_t>(data.reference)) == 0x14)) {
    // Preserve the existing context acceptance for predicates whose receiver
    // is the campaign journal/global/conditional script, not a physical actor.
    throw std::runtime_error("Unsupported quest condition run-on context");
  }
  espm::CTDA::DefaultData parameters{};
  std::memcpy(&parameters, data.functionData, sizeof(parameters));
  result.parameter2 = parameters.secondParameter;
  if (locationAlias || result.function == Function::IsAliasRef) {
    // The operand at byte 12 is another owning-quest alias ID, independent
    // of the run-on alias at byte 28. Neither is a plugin-relative FormID.
    if (parameters.firstParameter > INT_MAX)
      throw std::runtime_error(
        "Quest condition requires a nonnegative alias ID");
    result.parameter1 = parameters.firstParameter;
    if (locationAlias) {
      if (!parameters.secondParameter ||
          parameters.secondParameter >= 0xff000000)
        throw std::runtime_error(
          "Location condition requires a static operand");
      result.parameter2 = mapForm(parameters.secondParameter);
      if (!result.parameter2 || result.parameter2 >= 0xff000000)
        throw std::runtime_error(
          "Location condition requires a static operand");
    }
  } else if (result.function != Function::Disabled &&
             result.function != Function::Dead)
    result.parameter1 = mapForm(parameters.firstParameter);
  result.comparison = data.GetOperator();
  result.orNext = (data.operatorFlag & 1) != 0;
  if (data.operatorFlag & 4) {
    uint32_t raw;
    std::memcpy(&raw, &data.comparisonValue, sizeof(raw));
    if (!raw)
      throw std::runtime_error("Quest comparison global is null");
    result.comparisonGlobal = mapForm(raw);
    if (!*result.comparisonGlobal)
      throw std::runtime_error("Quest comparison global is null");
  } else {
    result.comparisonValue = static_cast<float>(Finite(data.comparisonValue));
  }
  return result;
}

void CampaignCondition::SetVariableName(std::string name)
{
  if (function != Function::QuestVariable || !variableName.empty() ||
      name.empty() || name.size() >= 65536 ||
      name.find('\0') != std::string::npos)
    throw std::runtime_error("Unexpected quest condition string");
  variableName = std::move(name);
}

void CampaignCondition::Validate() const
{
  if (function == Function::QuestVariable && variableName.empty())
    throw std::runtime_error("Missing quest condition variable name");
}

double CampaignCondition::Read(const Context& context) const
{
  if (function == Function::LocAliasIsLocation ||
      function == Function::LocAliasHasKeyword) {
    if (!context.resolveLocationAlias)
      throw std::runtime_error(
        "Quest condition requires a location alias resolver");
    const auto id =
      context.resolveLocationAlias(context.owningQuest, parameter1);
    if (!id)
      return 0;
    const auto location = StaticOperand(context.actor, id, "LCTN");
    // Both predicates inspect the aliased location itself. Parent locations
    // do not supply an identity match or inherited keywords.
    if (function == Function::LocAliasIsLocation)
      return id == parameter2;
    return LocationHasKeyword(*context.actor.GetParent(), location,
                              parameter2);
  }
  if (function == Function::IsAliasRef) {
    if (!context.resolveReferenceAlias)
      throw std::runtime_error(
        "GetIsAliasRef requires a reference alias resolver");
    const auto resolve = [&](uint32_t original) -> MpObjectReference* {
      if (!original)
        return nullptr;
      if (original >= 0xff000000)
        throw std::runtime_error(
          "Quest condition alias has no logical reference");
      ValidateReference(context.actor, original, false);
      auto& reference =
        CampaignReferenceBinding::Resolve(context.actor, original);
      // Track both identities through commit, including a false comparison.
      // Equality is reference identity, never NPC/item base or template ID.
      QuestJournal::ReadReference(context.actor, reference);
      return &reference;
    };
    auto* subject = resolve(
      selector == Selector::Alias
        ? context.resolveReferenceAlias(context.owningQuest, referenceOrAlias)
        : referenceOrAlias);
    auto* operand =
      resolve(context.resolveReferenceAlias(context.owningQuest, parameter1));
    // Vanilla compares reference handles and requires a nonnull receiver:
    // two empty aliases do not count as the same reference.
    return subject && subject == operand ? 1.0 : 0.0;
  }
  if (selector != Selector::None) {
    uint32_t original = referenceOrAlias;
    if (selector == Selector::Alias) {
      if (!context.resolveReferenceAlias)
        throw std::runtime_error(
          "Quest condition requires a reference alias resolver");
      original =
        context.resolveReferenceAlias(context.owningQuest, referenceOrAlias);
      if (!original || original >= 0xff000000)
        throw std::runtime_error(
          "Quest condition alias has no logical reference");
    }
    auto& reference =
      CampaignReferenceBinding::Resolve(context.actor, original);
    if (function == Function::InFaction || function == Function::FactionRank) {
      auto* target = reference.AsActor();
      if (!target)
        throw std::runtime_error(
          "Faction conditions require a campaign actor");
      if (function == Function::InFaction)
        return CampaignFactionApi::IsMember(context.actor, *target, parameter1)
          ? 1.0
          : 0.0;
      return static_cast<double>(
        CampaignFactionApi::GetRank(context.actor, *target, parameter1));
    }
    const auto& snapshot =
      QuestJournal::ReadReference(context.actor, reference);
    if (function == Function::Disabled)
      return snapshot.isDisabled ? 1.0 : 0.0;
    if (function == Function::Dead) {
      if (snapshot.recType != MpChangeFormREFR::ACHR)
        throw std::runtime_error("GetDead requires a campaign actor");
      return snapshot.isDead ? 1.0 : 0.0;
    }
    uint64_t count = 0;
    // Inventory reads additionally require initialized private containers;
    // a valid binding alone must not turn an uninitialized inventory into 0.
    for (const auto& entry :
         QuestJournal::ReadInventory(context.actor, reference).entries) {
      if (entry.baseId == parameter1)
        count += entry.count;
      if (count > INT_MAX)
        throw std::overflow_error(
          "Campaign condition inventory count overflow");
    }
    // Integer counts within the Papyrus range are exactly representable in a
    // double. A float would make 16777217 > 16777216 incorrectly false.
    return static_cast<double>(count);
  }
  if (function == Function::Global) {
    if (!context.readGlobal)
      throw std::runtime_error("Quest condition requires a global resolver");
    return context.readGlobal(parameter1);
  }
  if (function == Function::QuestVariable) {
    if (!context.readQuestVariable)
      throw std::runtime_error(
        "Quest variable condition requires CIS2 resolver");
    return context.readQuestVariable(parameter1, variableName);
  }
  const auto quest = QuestJournal::Get(context.actor, parameter1);
  switch (function) {
    case Function::QuestRunning:
      return quest.at("running").get<bool>() ? 1.0 : 0.0;
    case Function::Stage:
      return quest.at("stage").get<double>();
    case Function::QuestCompleted:
      return quest.at("completed").get<bool>() ? 1.0 : 0.0;
    case Function::StageDone: {
      const auto& stages = quest.at("stages");
      return std::find(stages.begin(), stages.end(), parameter2) !=
          stages.end()
        ? 1.0
        : 0.0;
    }
    default:
      throw std::logic_error("Invalid campaign condition");
  }
}

bool CampaignCondition::Compare(double actual, double expected) const
{
  Finite(actual);
  Finite(expected);
  switch (comparison) {
    case espm::CTDA::Operator::EqualTo:
      return actual == expected;
    case espm::CTDA::Operator::NotEqualTo:
      return actual != expected;
    case espm::CTDA::Operator::GreaterThen:
      return actual > expected;
    case espm::CTDA::Operator::GreaterThenOrEqualTo:
      return actual >= expected;
    case espm::CTDA::Operator::LessThen:
      return actual < expected;
    case espm::CTDA::Operator::LessThenOrEqualTo:
      return actual <= expected;
  }
  throw std::logic_error("Invalid campaign condition comparison");
}

bool CampaignCondition::Evaluate(
  const std::vector<CampaignCondition>& conditions, const Context& context)
{
  if (!QuestJournal::HasActiveTransaction())
    throw std::runtime_error(
      "Campaign conditions require a quest transaction");
  std::vector<double> comparisons;
  comparisons.reserve(conditions.size());
  for (const auto& condition : conditions) {
    condition.Validate();
    // Validate static record kinds even behind a successful OR arm. Resolve
    // private copies and aliases only when their branch actually executes.
    if (condition.selector == Selector::Reference)
      ValidateReference(context.actor, condition.referenceOrAlias,
                        condition.function == Function::Dead ||
                          condition.function == Function::InFaction ||
                          condition.function == Function::FactionRank);
    if (condition.function == Function::ItemCount)
      ValidateItem(context.actor, condition.parameter1);
    if (condition.function == Function::LocAliasIsLocation)
      StaticOperand(context.actor, condition.parameter2, "LCTN");
    if (condition.function == Function::LocAliasHasKeyword)
      StaticOperand(context.actor, condition.parameter2, "KYWD");
    if (condition.function == Function::InFaction ||
        condition.function == Function::FactionRank)
      CampaignFactionApi::ValidateFaction(context.actor, condition.parameter1);
    if (condition.comparisonGlobal) {
      if (!context.readGlobal)
        throw std::runtime_error(
          "Quest comparison requires a global resolver");
      comparisons.push_back(
        Finite(context.readGlobal(*condition.comparisonGlobal)));
    } else {
      comparisons.push_back(condition.comparisonValue);
    }
  }
  // Consecutive ORs form a group; groups are ANDed, matching Skyrim CTDA.
  bool group = false;
  for (size_t i = 0; i < conditions.size(); ++i) {
    const auto& condition = conditions[i];
    if (!group)
      group = condition.Compare(condition.Read(context), comparisons[i]);
    if (!condition.orNext || i + 1 == conditions.size()) {
      if (!group)
        return false;
      group = false;
    }
  }
  return true;
}
