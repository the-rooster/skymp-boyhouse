#include "HeuristicPolicy.h"
#include "QuestJournal.h"
#include "WorldState.h"

#include "script_objects/MpFormGameObject.h"

HeuristicPolicy::HeuristicPolicy(WorldState* worldState_)
  : worldState(worldState_)
{
}

void HeuristicPolicy::SetDefaultActor(int32_t stackId, MpActor* newActor)
{
  if (stackId < 0) {
    throw std::runtime_error("Invalid stackId was passed to SetDefaultActor");
  }

  if (stackInfo.size() <= stackId) {
    stackInfo.resize(stackId + 1);
  }
  stackInfo[stackId].ac = newActor;
  stackInfo[stackId].actorId = newActor ? newActor->GetFormId() : 0;
  stackInfo[stackId].instanceEpoch =
    newActor ? newActor->GetWorldInstanceEpoch() : 0;
  auto& info = stackInfo[stackId];
  info.actorLifetime.reset();
  info.ownerLifetime.reset();
  if (newActor && newActor->GetParent()) {
    info.actorLifetime =
      worldState->LookupFormByIdNoLoad(newActor->GetFormId());
    try {
      auto& owner = QuestJournal::Owner(*newActor);
      info.ownerLifetime = worldState->LookupFormByIdNoLoad(owner.GetFormId());
      info.ownerEpoch = owner.GetWorldInstanceEpoch();
    } catch (const std::exception&) {
      // Existing non-campaign scripts still get their default actor. Quest
      // calls fail closed if no campaign context existed when the stack began.
    }
  }
}

MpActor* HeuristicPolicy::GetDefaultActor(const char* className,
                                          const char* funcName,
                                          int32_t stackId) const
{
  if (stackId < 0 || stackId >= stackInfo.size()) {
    throw std::runtime_error(
      "Invalid stackId was passed to GetDefaultActor (" +
      std::to_string(stackId) + ")");
  }

  auto& info = stackInfo[stackId];

  MpActor* actor = stackId < stackInfo.size() ? stackInfo[stackId].ac
                                              : static_cast<MpActor*>(nullptr);

  if (!actor) {
    spdlog::warn("Unable to determine Actor for '{}.{}' in '{}'", className,
                 funcName, info.currentEventName);
  }
  return actor;
}

WorldState* HeuristicPolicy::GetWorldState() const
{
  return worldState;
}

void HeuristicPolicy::BeforeSendPapyrusEvent(MpForm* form,
                                             const char* eventName,
                                             const VarValue* arguments,
                                             size_t argumentsCount,
                                             int32_t stackId)
{
  MpActor* actor = nullptr;

  if (!Utils::stricmp(eventName, "OnActivate") && argumentsCount >= 1) {
    actor = GetFormPtr<MpActor>(arguments[0]);
  } else if (!Utils::stricmp(eventName, "OnObjectEquipped") ||
             !Utils::stricmp(eventName, "OnInit") ||
             !Utils::stricmp(eventName, "OnUpdate")) {
    actor = form->AsActor();
  } else if ((!Utils::stricmp(eventName, "OnTriggerEnter") ||
              !Utils::stricmp(eventName, "OnTriggerLeave") ||
              !Utils::stricmp(eventName, "OnTrigger")) &&
             argumentsCount >= 1) {
    actor = GetFormPtr<MpActor>(arguments[0]);
  } else if (!Utils::stricmp(eventName, "OnHit") && argumentsCount >= 1) {
    actor = GetFormPtr<MpActor>(arguments[0]);
  }

  if (stackInfo.size() <= stackId) {
    stackInfo.resize(stackId + 1);
  }
  stackInfo[stackId] = { actor, eventName };
  SetDefaultActor(stackId, actor);
}

MpActor* HeuristicPolicy::GetQuestActor(int32_t stackId) const
{
  if (stackId < 0 || static_cast<size_t>(stackId) >= stackInfo.size()) {
    throw std::runtime_error("Quest call has no campaign execution context");
  }
  const auto& info = stackInfo[stackId];
  const auto& form = worldState->LookupFormByIdNoLoad(info.actorId);
  auto actor = form ? form->AsActor() : nullptr;
  if (!actor || actor != info.actorLifetime.lock().get() ||
      actor->GetWorldInstanceEpoch() != info.instanceEpoch) {
    throw std::runtime_error(
      "Quest call belongs to an expired campaign context");
  }
  auto& owner = QuestJournal::Owner(*actor);
  if (&owner != info.ownerLifetime.lock().get() ||
      owner.GetWorldInstanceEpoch() != info.ownerEpoch) {
    throw std::runtime_error("Quest campaign owner context has expired");
  }
  return actor;
}
