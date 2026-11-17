#pragma once
#include "libespm/CTDA.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

class MpActor;

// Conditions for the isolated quest runtime. A log has an owning quest, but
// no implicit physical Subject/Target. Other callers (dialogue, alias filling,
// packages) must supply their own context before those selectors are
// supported.
class CampaignCondition
{
public:
  using MapForm = std::function<uint32_t(uint32_t)>;
  struct Context
  {
    MpActor& actor;
    uint32_t owningQuest;
    std::function<double(uint32_t)> readGlobal;
    std::function<double(uint32_t, const std::string&)> readQuestVariable;
    // Resolve through the owning quest's existing reference-alias lifecycle.
    // Return a logical original, never a physical copy ID. Zero means a proven
    // empty alias; missing definitions and unsupported fill modes must throw.
    std::function<uint32_t(uint32_t, uint32_t)> resolveReferenceAlias;
    // Returns a static LCTN for the owning quest's location alias, or zero
    // for a proven empty binding.
    std::function<uint32_t(uint32_t, uint32_t)> resolveLocationAlias{};
  };

  // Compile every CTDA before evaluating any branch. Unsupported functions,
  // flags, and selectors must not be hidden by OR short-circuiting.
  static CampaignCondition Compile(const espm::CTDA& data,
                                   const MapForm& mapForm);
  void SetVariableName(std::string name);
  void Validate() const;
  static bool Evaluate(const std::vector<CampaignCondition>& conditions,
                       const Context& context);

private:
  enum class Function : uint16_t
  {
    Disabled = 35,
    Dead = 46,
    ItemCount = 47,
    QuestRunning = 56,
    Stage = 58,
    StageDone = 59,
    InFaction = 71,
    FactionRank = 73,
    Global = 74,
    QuestCompleted = 543,
    IsAliasRef = 566,
    LocAliasIsLocation = 605,
    LocAliasHasKeyword = 610,
    QuestVariable = 629
  };
  enum class Selector
  {
    None,
    Reference,
    Alias
  };

  CampaignCondition() = default;
  double Read(const Context& context) const;
  bool Compare(double actual, double expected) const;

  Function function;
  Selector selector = Selector::None;
  uint32_t referenceOrAlias = 0;
  uint32_t parameter1 = 0;
  uint32_t parameter2 = 0;
  std::string variableName;
  espm::CTDA::Operator comparison;
  float comparisonValue = 0;
  std::optional<uint32_t> comparisonGlobal;
  bool orNext = false;
};
