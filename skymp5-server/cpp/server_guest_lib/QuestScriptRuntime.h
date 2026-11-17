#pragma once
#include "QuestJobState.h"
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <nlohmann/json.hpp>

class MpActor;
class MpObjectReference;
class WorldState;

class QuestScriptRuntime
{
public:
  static std::vector<PexScript::Lazy> LoadScripts(WorldState& world);
  // Called within the coordinator's AtomicJob segment. The caller persists
  // the updated job alongside the heap; this function never dispatches an
  // awaited operation. Fixture awaits are explicitly opt-in test injection.
  static void ExecuteJobSegment(
    MpActor& actor, QuestJobState::Job& job,
    const espm::QuestScriptData& metadata,
    const std::vector<PexScript::Lazy>& scripts,
    std::optional<bool> completion = {}, bool fixtureAwait = false,
    const std::map<uint32_t, espm::QuestScriptData>& overrides = {},
    const std::string& contentManifest = {});
  // Trusted experimental stage execution. The quest adapter remains
  // responsible for startup, alias filling and external effects. Unsupported
  // natives throw and roll back journal/VM state. No client packet invokes
  // this API.
  static bool ExecuteStage(MpActor& actor, uint32_t questId, uint16_t stage);

  // Explicit dependency injection for deterministic VM fixtures. Production
  // uses owned QUST metadata and PEX storage through the overload above.
  static bool ExecuteStage(MpActor& actor, uint32_t questId, uint16_t stage,
                           const espm::QuestScriptData& metadata,
                           const std::vector<PexScript::Lazy>& scripts);

  // One reference load transaction: OnInit once per campaign identity, then
  // OnCellLoad/OnLoad. The reference must already have a private world copy.
  static void ExecuteReferenceLoad(MpActor& actor, uint32_t original);
  static void ExecuteReferenceLoad(
    MpActor& actor, uint32_t original,
    const std::vector<PexScript::Lazy>& scripts);

  // Synchronous native interaction ingress. Only supported one-reference
  // events are accepted; native objects never enter the isolated VM. Guests
  // act as PlayerRef in the owner's history, just like Game.GetPlayer().
  static void ExecuteReferenceEvent(MpObjectReference& target,
                                    const char* eventName,
                                    const VarValue* arguments, size_t count);
  static void ExecuteReferenceEvent(
    MpObjectReference& target, const char* eventName,
    const VarValue* arguments, size_t count,
    const std::vector<PexScript::Lazy>& scripts);

private:
  static bool Execute(
    MpActor& actor, uint32_t questId, uint16_t stage,
    const espm::QuestScriptData& metadata,
    const std::vector<PexScript::Lazy>& scripts, uint32_t loadedReference = 0,
    const char* referenceEvent = nullptr, uint32_t eventSource = 0,
    QuestJobState::Job* job = nullptr, std::optional<bool> completion = {},
    bool fixtureAwait = false,
    const std::map<uint32_t, espm::QuestScriptData>& overrides = {},
    const std::string& contentManifest = {});
};
