#pragma once
#include "ConditionFunction.h"
#include "QuestJournal.h"
#include <algorithm>

namespace ConditionFunctions {
// Indices verified against TES5Edit's Core/wbDefinitionsTES5.pas (dev-4.1.6).
// Evaluates the selected actor's active campaign. Dialogue/scene run-on and
// alias selection still need their own execution context before being enabled.
class QuestCondition : public ConditionFunction
{
public:
  enum class Kind
  {
    Running,
    Stage,
    StageDone,
    Completed
  };
  explicit QuestCondition(Kind kind_)
    : kind(kind_)
  {
  }

  const char* GetName() const override
  {
    switch (kind) {
      case Kind::Running:
        return "GetQuestRunning";
      case Kind::Stage:
        return "GetStage";
      case Kind::StageDone:
        return "GetStageDone";
      case Kind::Completed:
        return "GetQuestCompleted";
    }
    throw std::logic_error("Invalid quest condition");
  }

  uint16_t GetFunctionIndex() const override
  {
    switch (kind) {
      case Kind::Running:
        return 56;
      case Kind::Stage:
        return 58;
      case Kind::StageDone:
        return 59;
      case Kind::Completed:
        return 543;
    }
    throw std::logic_error("Invalid quest condition");
  }

  float Execute(MpActor& actor, uint32_t questId, uint32_t stage,
                const ConditionEvaluatorContext&) override
  {
    const auto quest = QuestJournal::Get(actor, questId);
    switch (kind) {
      case Kind::Running:
        return quest.at("running").get<bool>() ? 1.f : 0.f;
      case Kind::Stage:
        return quest.at("stage").get<float>();
      case Kind::Completed:
        return quest.at("completed").get<bool>() ? 1.f : 0.f;
      case Kind::StageDone: {
        const auto& stages = quest.at("stages");
        return std::find(stages.begin(), stages.end(), stage) != stages.end()
          ? 1.f
          : 0.f;
      }
    }
    throw std::logic_error("Invalid quest condition");
  }

private:
  Kind kind;
};
}
