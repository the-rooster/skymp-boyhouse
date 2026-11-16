#pragma once
#include "IPapyrusCompatibilityPolicy.h"

#include "MpActor.h"
#include "papyrus-vm/Utils.h"
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/MpFormGameObject.h"
#include <vector>

class HeuristicPolicy : public IPapyrusCompatibilityPolicy
{
public:
  explicit HeuristicPolicy(WorldState* worldState_);

  MpActor* GetDefaultActor(const char* className, const char* funcName,
                           int32_t stackId) const override;

  WorldState* GetWorldState() const override;
  MpActor* GetQuestActor(int32_t stackId) const override;

  void SetDefaultActor(int32_t stackId, MpActor* actor) override;

  void BeforeSendPapyrusEvent(MpForm* form, const char* eventName,
                              const VarValue* arguments, size_t argumentsCount,
                              int32_t stackId) override;

private:
  struct StackInfo
  {
    MpActor* ac = nullptr;
    const char* currentEventName = "";
    uint32_t actorId = 0;
    uint64_t instanceEpoch = 0;
    std::weak_ptr<MpForm> actorLifetime;
    std::weak_ptr<MpForm> ownerLifetime;
    uint64_t ownerEpoch = 0;
  };
  std::vector<StackInfo> stackInfo;
  WorldState* const worldState;
};
