#pragma once
#include "QuestJobClock.h"
#include "QuestJobState.h"
#include "QuestJournal.h"
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <nlohmann/json.hpp>

class MpActor;
class MpObjectReference;
class WorldState;
namespace espm {
class StoryEventRouteIndex;
}

class QuestScriptRuntime
{
public:
  static std::vector<PexScript::Lazy> LoadScripts(WorldState& world);
  // Capture only registered, already-materialized private references. These
  // helpers do not create copies or submit work. Validation inside AtomicJob
  // also tracks target/NPC source reads; player source remains the initiator.
  static QuestJobState::Interaction CaptureInteraction(
    MpActor& actor, MpObjectReference& target, const std::string& name,
    MpObjectReference& source);
  static QuestJobState::Interaction CaptureInteraction(
    MpActor& actor, MpObjectReference& target, const std::string& name,
    MpObjectReference& source, const QuestJournal::CaptureContext& context);
  static void ValidateInteraction(MpActor& actor,
                                  const QuestJobState::Interaction& event);
  // Load work belongs to the campaign owner and may precede initialization.
  // Capture/validation preserve the physical target and its chosen templates.
  static QuestJobState::ReferenceLoad CaptureReferenceLoad(
    MpActor& actor, MpObjectReference& target);
  static void ValidateReferenceLoad(MpActor& actor,
                                    const QuestJobState::ReferenceLoad& load);
  // Called within the selected root's AtomicJob lease. Other live roots share
  // the current campaign heap but retain their own saved stacks and budgets.
  // The caller persists the updated job alongside the heap. It never
  // dispatches an awaited operation. Fixture awaits are explicitly opt-in test
  // injection. timerAnchor is provisional output: publish it only after
  // AtomicJob succeeds, and never resume it before that segment's durable
  // receipt.
  static void ExecuteJobSegment(
    MpActor& actor, QuestJobState::Job& job,
    const espm::QuestScriptData& metadata,
    const std::vector<PexScript::Lazy>& scripts,
    std::optional<QuestJobState::Completion> completion = {},
    bool fixtureAwait = false,
    const std::map<uint32_t, espm::QuestScriptData>& overrides = {},
    const std::string& contentManifest = {}, const QuestJobClock& clock = {},
    std::optional<QuestJobTimerAnchor>* timerAnchor = nullptr);
  // Trusted experimental stage execution. The quest adapter remains
  // responsible for startup, alias filling and external effects. Unsupported
  // natives throw and roll back journal/VM state. No client packet invokes
  // this API.
  static bool ExecuteStage(MpActor& actor, uint32_t questId, uint16_t stage);

  // Exercise a first start in an isolated, pristine campaign. The return
  // records reached phases; the transaction is discarded even on success.
  struct ActorDialogue
  {
    MpActor& actor1; // Initiating NPC
    MpActor& actor2; // Addressed NPC
    const espm::StoryEventRouteIndex& routes;
  };
  static nlohmann::json ProbeStartup(
    MpActor& actor, uint32_t questId, uint32_t startScene,
    const std::vector<PexScript::Lazy>& scripts,
    const ActorDialogue* dialogue = nullptr);

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
  struct StartupProbe;
  static bool Execute(
    MpActor& actor, uint32_t questId, uint16_t stage,
    const espm::QuestScriptData& metadata,
    const std::vector<PexScript::Lazy>& scripts, uint32_t loadedReference = 0,
    const char* referenceEvent = nullptr, uint32_t eventSource = 0,
    QuestJobState::Job* job = nullptr,
    std::optional<QuestJobState::Completion> completion = {},
    bool fixtureAwait = false,
    const std::map<uint32_t, espm::QuestScriptData>& overrides = {},
    const std::string& contentManifest = {}, const QuestJobClock& clock = {},
    std::optional<QuestJobTimerAnchor>* timerAnchor = nullptr,
    StartupProbe* startup = nullptr);
};
