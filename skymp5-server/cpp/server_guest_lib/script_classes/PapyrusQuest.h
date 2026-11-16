#pragma once
#include "IPapyrusClass.h"
#include <nlohmann/json.hpp>

class PapyrusQuest final : public IPapyrusClass<PapyrusQuest>
{
public:
  const char* GetName() override { return "Quest"; }
  VarValue GetStage(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetStageDone(VarValue self, const std::vector<VarValue>& arguments);
  VarValue IsStageDone(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetStage(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetCurrentStageID(VarValue self,
                             const std::vector<VarValue>& arguments);
  VarValue IsStopped(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetActive(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetCurrentStageID(VarValue self,
                             const std::vector<VarValue>& arguments);
  VarValue IsRunning(VarValue self, const std::vector<VarValue>& arguments);
  VarValue IsCompleted(VarValue self, const std::vector<VarValue>& arguments);
  VarValue IsActive(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Start(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Stop(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Reset(VarValue self, const std::vector<VarValue>& arguments);
  VarValue CompleteQuest(VarValue self,
                         const std::vector<VarValue>& arguments);
  VarValue CompleteAllObjectives(VarValue self,
                                 const std::vector<VarValue>& arguments);
  VarValue FailAllObjectives(VarValue self,
                             const std::vector<VarValue>& arguments);
  VarValue IsObjectiveDisplayed(VarValue self,
                                const std::vector<VarValue>& arguments);
  VarValue IsObjectiveCompleted(VarValue self,
                                const std::vector<VarValue>& arguments);
  VarValue IsObjectiveFailed(VarValue self,
                             const std::vector<VarValue>& arguments);
  VarValue SetObjectiveDisplayed(VarValue self,
                                 const std::vector<VarValue>& arguments);
  VarValue SetObjectiveCompleted(VarValue self,
                                 const std::vector<VarValue>& arguments);
  VarValue SetObjectiveFailed(VarValue self,
                              const std::vector<VarValue>& arguments);
  void Register(VirtualMachine& vm,
                std::shared_ptr<IPapyrusCompatibilityPolicy> policy) override;

private:
  MpActor& Actor(const VarValue& self) const;
  uint32_t Id(const VarValue& self) const;
  nlohmann::json Read(const VarValue& self) const;
  bool Apply(const VarValue& self, nlohmann::json command) const;
};
