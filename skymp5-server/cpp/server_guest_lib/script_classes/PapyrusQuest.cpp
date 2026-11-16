#include "PapyrusQuest.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "script_objects/CampaignQuestObject.h"
#include "script_objects/EspmGameObject.h"
#include <algorithm>

MpActor& PapyrusQuest::Actor(const VarValue& self) const
{
  if (!compatibilityPolicy)
    throw std::runtime_error("Missing quest campaign policy");
  auto actor = compatibilityPolicy->GetQuestActor(self.GetMetaStackId());
  if (!actor)
    throw std::runtime_error("Missing quest campaign actor");
  if (self.GetType() == VarValue::kType_Object) {
    if (auto scoped = dynamic_cast<CampaignQuestObject*>(
          static_cast<IGameObject*>(self))) {
      if (scoped->OwnerId() != QuestJournal::Owner(*actor).GetFormId())
        throw std::runtime_error("Quest object belongs to another campaign");
    }
  }
  return *actor;
}

uint32_t PapyrusQuest::Id(const VarValue& self) const
{
  const auto& record = GetRecordPtr(self);
  if (!record.rec || record.rec->GetType() != "QUST")
    throw std::invalid_argument("Expected quest self");
  return record.ToGlobalId(record.rec->GetId());
}

nlohmann::json PapyrusQuest::Read(const VarValue& self) const
{
  return QuestJournal::Get(Actor(self), Id(self));
}

bool PapyrusQuest::Apply(const VarValue& self, nlohmann::json command) const
{
  command["quest"] = Id(self);
  return QuestJournal::Apply(Actor(self), command);
}

namespace {
uint32_t Index(const std::vector<VarValue>& args)
{
  if (args.empty() || args[0].GetType() != VarValue::kType_Integer)
    throw std::invalid_argument("Quest index must be an integer");
  const int32_t index = static_cast<int32_t>(args[0]);
  if (index < 0 || index > 65535)
    throw std::invalid_argument("Quest index out of range");
  return index;
}
bool Flag(const std::vector<VarValue>& args, size_t index, bool fallback)
{
  if (args.size() <= index)
    return fallback;
  if (args[index].GetType() != VarValue::kType_Bool)
    throw std::invalid_argument("Quest flag must be boolean");
  return static_cast<bool>(args[index]);
}
}

VarValue PapyrusQuest::GetCurrentStageID(
  VarValue self, const std::vector<VarValue>& arguments)
{
  return VarValue(Read(self).at("stage").get<int32_t>());
}

VarValue PapyrusQuest::IsRunning(VarValue self,
                                 const std::vector<VarValue>& arguments)
{
  return VarValue(Read(self).at("running").get<bool>());
}

VarValue PapyrusQuest::IsCompleted(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  return VarValue(Read(self).at("completed").get<bool>());
}

VarValue PapyrusQuest::IsActive(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  return VarValue(Read(self).at("active").get<bool>());
}

VarValue PapyrusQuest::GetStage(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  return GetCurrentStageID(self, arguments);
}

VarValue PapyrusQuest::IsStopped(VarValue self,
                                 const std::vector<VarValue>& arguments)
{
  return VarValue(!Read(self).at("running").get<bool>());
}

VarValue PapyrusQuest::IsStageDone(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  const auto index = Index(arguments);
  const auto stages = Read(self).at("stages");
  return VarValue(std::find(stages.begin(), stages.end(), index) !=
                  stages.end());
}

VarValue PapyrusQuest::GetStageDone(VarValue self,
                                    const std::vector<VarValue>& arguments)
{
  return IsStageDone(self, arguments);
}

VarValue PapyrusQuest::SetStage(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  return SetCurrentStageID(self, arguments);
}

VarValue PapyrusQuest::SetCurrentStageID(
  VarValue self, const std::vector<VarValue>& arguments)
{
  return VarValue(
    Apply(self, { { "op", "setStage" }, { "index", Index(arguments) } }));
}

VarValue PapyrusQuest::SetActive(VarValue self,
                                 const std::vector<VarValue>& arguments)
{
  Apply(self,
        { { "op", "setActive" }, { "value", Flag(arguments, 0, true) } });
  return VarValue::None();
}

VarValue PapyrusQuest::Start(VarValue self,
                             const std::vector<VarValue>& arguments)
{
  return VarValue(Apply(self, { { "op", "start" } }));
}

VarValue PapyrusQuest::Stop(VarValue self,
                            const std::vector<VarValue>& arguments)
{
  Apply(self, { { "op", "stop" } });
  return VarValue::None();
}

VarValue PapyrusQuest::Reset(VarValue self,
                             const std::vector<VarValue>& arguments)
{
  Apply(self, { { "op", "reset" } });
  return VarValue::None();
}

VarValue PapyrusQuest::CompleteQuest(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  Apply(self, { { "op", "complete" } });
  return VarValue::None();
}

VarValue PapyrusQuest::CompleteAllObjectives(
  VarValue self, const std::vector<VarValue>& arguments)
{
  Apply(self, { { "op", "completeAllObjectives" } });
  return VarValue::None();
}

VarValue PapyrusQuest::FailAllObjectives(
  VarValue self, const std::vector<VarValue>& arguments)
{
  Apply(self, { { "op", "failAllObjectives" } });
  return VarValue::None();
}

VarValue PapyrusQuest::IsObjectiveDisplayed(
  VarValue self, const std::vector<VarValue>& arguments)
{
  const auto index = std::to_string(Index(arguments));
  const auto objectives = Read(self).at("objectives");
  return VarValue(objectives.contains(index) &&
                  objectives.at(index).at("displayed") == true);
}

VarValue PapyrusQuest::IsObjectiveCompleted(
  VarValue self, const std::vector<VarValue>& arguments)
{
  const auto index = std::to_string(Index(arguments));
  const auto objectives = Read(self).at("objectives");
  return VarValue(objectives.contains(index) &&
                  objectives.at(index).at("completed") == true);
}

VarValue PapyrusQuest::IsObjectiveFailed(
  VarValue self, const std::vector<VarValue>& arguments)
{
  const auto index = std::to_string(Index(arguments));
  const auto objectives = Read(self).at("objectives");
  return VarValue(objectives.contains(index) &&
                  objectives.at(index).at("failed") == true);
}

VarValue PapyrusQuest::SetObjectiveDisplayed(
  VarValue self, const std::vector<VarValue>& arguments)
{
  Apply(self,
        { { "op", "setObjectiveDisplayed" },
          { "index", Index(arguments) },
          { "value", Flag(arguments, 1, true) } });
  return VarValue::None();
}

VarValue PapyrusQuest::SetObjectiveCompleted(
  VarValue self, const std::vector<VarValue>& arguments)
{
  Apply(self,
        { { "op", "setObjectiveCompleted" },
          { "index", Index(arguments) },
          { "value", Flag(arguments, 1, true) } });
  return VarValue::None();
}

VarValue PapyrusQuest::SetObjectiveFailed(
  VarValue self, const std::vector<VarValue>& arguments)
{
  Apply(self,
        { { "op", "setObjectiveFailed" },
          { "index", Index(arguments) },
          { "value", Flag(arguments, 1, true) } });
  return VarValue::None();
}

void PapyrusQuest::Register(
  VirtualMachine& vm, std::shared_ptr<IPapyrusCompatibilityPolicy> policy)
{
  compatibilityPolicy = policy;
  AddMethod(vm, "GetStage", &PapyrusQuest::GetStage);
  AddMethod(vm, "GetStageDone", &PapyrusQuest::GetStageDone);
  AddMethod(vm, "IsStageDone", &PapyrusQuest::IsStageDone);
  AddMethod(vm, "SetStage", &PapyrusQuest::SetStage);
  AddMethod(vm, "SetCurrentStageID", &PapyrusQuest::SetCurrentStageID);
  AddMethod(vm, "IsStopped", &PapyrusQuest::IsStopped);
  AddMethod(vm, "SetActive", &PapyrusQuest::SetActive);
  AddMethod(vm, "GetCurrentStageID", &PapyrusQuest::GetCurrentStageID);
  AddMethod(vm, "IsRunning", &PapyrusQuest::IsRunning);
  AddMethod(vm, "IsCompleted", &PapyrusQuest::IsCompleted);
  AddMethod(vm, "IsActive", &PapyrusQuest::IsActive);
  AddMethod(vm, "Start", &PapyrusQuest::Start);
  AddMethod(vm, "Stop", &PapyrusQuest::Stop);
  AddMethod(vm, "Reset", &PapyrusQuest::Reset);
  AddMethod(vm, "CompleteQuest", &PapyrusQuest::CompleteQuest);
  AddMethod(vm, "CompleteAllObjectives", &PapyrusQuest::CompleteAllObjectives);
  AddMethod(vm, "FailAllObjectives", &PapyrusQuest::FailAllObjectives);
  AddMethod(vm, "IsObjectiveDisplayed", &PapyrusQuest::IsObjectiveDisplayed);
  AddMethod(vm, "IsObjectiveCompleted", &PapyrusQuest::IsObjectiveCompleted);
  AddMethod(vm, "IsObjectiveFailed", &PapyrusQuest::IsObjectiveFailed);
  AddMethod(vm, "SetObjectiveDisplayed", &PapyrusQuest::SetObjectiveDisplayed);
  AddMethod(vm, "SetObjectiveCompleted", &PapyrusQuest::SetObjectiveCompleted);
  AddMethod(vm, "SetObjectiveFailed", &PapyrusQuest::SetObjectiveFailed);
}
