#pragma once
#include "libespm/LookupResult.h"
#include "libespm/QuestLifecycleData.h"
#include <string>
#include <vector>

class MpActor;

struct ActorDialogueConditionResult
{
  enum class State
  {
    Passed,
    Failed,
    Unsupported
  };
  State state;
  std::string detail;
};

// The caller supplies the two validated event actors and their shared current
// location. Each CTDA operand belongs to the plugin containing the condition.
ActorDialogueConditionResult EvaluateActorDialogueConditions(
  MpActor& actor1, MpActor& actor2, uint32_t locationId,
  const espm::LookupResult& record,
  const std::vector<espm::QuestLifecycleData::Condition>& conditions);
