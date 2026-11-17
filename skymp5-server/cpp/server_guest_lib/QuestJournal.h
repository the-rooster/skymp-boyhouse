#pragma once
#include "Faction.h"
#include "QuestJobState.h"
#include "SaveReceipt.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

class MpActor;
class MpObjectReference;
class Inventory;
class MpChangeFormREFR;
class CampaignMovementPlan;
class QuestScriptRuntime;

// Campaign state shared by Papyrus and the trusted gamemode API. Clients may
// read snapshots but cannot issue mutations. Original stage fragments/alias
// filling must be supplied by a quest adapter before a quest is enabled.
class QuestJournal
{
public:
  static constexpr auto kProperty = "private.boyhouse.questJournal";
  static constexpr auto kEnabled = "private.boyhouse.questAdapters";
  static MpActor& Owner(MpActor& actor);
  static nlohmann::json Snapshot(MpActor& actor);
  static nlohmann::json Get(MpActor& actor, uint32_t questId);
  static void RequireAdapter(MpActor& actor, uint32_t questId);
  static bool Apply(MpActor& actor, const nlohmann::json& command);
  // Record an intentional execution of an already-visited authored repeatable
  // stage. Only the isolated runner's active transaction may call this. Even
  // a pure reward/no-op fragment advances revision without duplicating stages.
  static void RecordRepeatedStage(MpActor& actor, uint32_t questId,
                                  uint16_t stage);
  // Synchronous journal/VM, inventory, actor factions and reference-visibility
  // transaction. Other world effects still require transactional adapters.
  static void Atomic(MpActor& actor, const std::function<void()>& execute);
  // Publishes the same transaction and seals its exact save batch. The receipt
  // stays pending until that batch commits; errors retry the retained
  // snapshot. Requires atomic storage with per-submission results. Even a
  // no-op seals the owner's state. A receipt is process-local, not durable job
  // deduplication.
  static std::shared_ptr<const SaveReceipt> AtomicDurable(
    MpActor& actor, const std::function<void()>& execute);
  static std::shared_ptr<const SaveReceipt> AtomicJob(
    MpActor& actor, uint64_t jobId, const std::function<void()>& execute);
  // Admission-only leases for contiguous new native occurrences. Each
  // initializer must install a pristine root and save only its first recipient
  // snapshot. Initializers cannot execute handlers or change gameplay state.
  // Prepare alone may attach a physical movement plan with StageMovement.
  // Empty batches consume no job identities and may still save movement.
  static std::shared_ptr<const SaveReceipt> AtomicJobAdmissions(
    MpActor& actor, const std::vector<uint64_t>& jobIds,
    const std::function<void()>& prepare,
    const std::function<void(uint64_t)>& initializeOne);
  struct AdmissionBatchCommit
  {
    std::shared_ptr<const SaveReceipt> receipt;
    std::vector<uint64_t> retiredIds;
  };
  // Journal-owned builder for native admission. Specs are copied and validated
  // before preparation. Each callback receives only its selected local root;
  // it must run ExecuteJobSegment once and increment segment. Generic
  // SetJobState is unavailable. Only the successful runtime recipient snapshot
  // is accepted; the assembled campaign is fully validated before save.
  static AdmissionBatchCommit AtomicJobAdmissionRoots(
    MpActor& actor, const std::vector<QuestJobState::Job>& pristineRoots,
    const std::function<void()>& prepare,
    const std::function<void(QuestJobState::Job&)>& captureOne);
  // Exactly one physical participant, staged only by the active requester's
  // admission prepare callback. This grants no source inventory or personal
  // journal authority; the world publishes it with the common save receipt.
  static void StageMovement(MpActor& requester,
                            const std::shared_ptr<CampaignMovementPlan>& plan);
  // Results are owned copies. Active transactions reuse their validated job
  // snapshot internally; reads outside a transaction decode current storage.
  static QuestJobState JobState(MpActor& actor);
  static QuestJobState PersonalJobState(MpActor& owner);
  static void SetJobState(MpActor& actor, const QuestJobState& state);
  // A saved live root is not itself an execution lease. Runtime entry must
  // match the selected job of the current campaign transaction.
  static void RequireJobExecution(MpActor& actor, uint64_t jobId);
  // Runtime entry checks the actual cursor, so an admission lease cannot be
  // reused to execute a handler before its initial snapshot is durable.
  static void RequireJobExecution(MpActor& actor,
                                  const QuestJobState::Job& job);
  // Only quest/journal mutation is leased; ordinary physical gameplay is not.
  static void RequireJobAccess(MpActor& actor);
  // Personal allocation/recovery must not borrow the visited host's lease.
  static void RequirePersonalJobAccess(MpActor& owner);
  static bool HasActiveTransaction() noexcept;
  static const Inventory& ReadInventory(MpActor& actor,
                                        MpObjectReference& target);
  static void EditInventory(MpActor& actor, MpObjectReference& target,
                            const std::function<void(Inventory&)>& edit);
  // Snapshot physical state without loading a shared original. The owner and
  // explicitly bound private copies are the only permitted reference targets.
  // Commit rejects changes to the observed native identity, phase or state.
  static const MpChangeFormREFR& ReadReference(MpActor& actor,
                                               MpObjectReference& target);
  static void SetReferenceDisabled(MpActor& actor, MpObjectReference& target,
                                   bool disabled);
  // Effective saved actor factions; absent snapshots are derived from
  // immutable NPC/template metadata inside the transaction, never lazy-loaded
  // into live actors. Explicit saved empty snapshots remain empty.
  static const std::vector<Faction>& ReadFactions(MpActor& actor,
                                                  MpObjectReference& target);
  static void EditFactions(
    MpActor& actor, MpObjectReference& target,
    const std::function<void(std::vector<Faction>&)>& edit);
  static nlohmann::json ScriptState(MpActor& actor);
  // Trusted allocation/recovery must read this character's own history even
  // while the character is visiting somebody else's campaign.
  static nlohmann::json PersonalScriptState(MpActor& actor);
  static void SetScriptState(MpActor& actor, const nlohmann::json& state);
  static bool ReferenceInitialized(MpActor& actor, uint32_t original);
  static void MarkReferenceInitialized(MpActor& actor, uint32_t original);

private:
  friend class QuestScriptRuntime;
  // Successful runtime return is the only publisher of a builder capture.
  // Generic execution leases do not use this admission-only handshake.
  static void CompleteAdmissionCapture(MpActor& actor,
                                       const QuestJobState::Job& job);
  static std::shared_ptr<const SaveReceipt> AtomicImpl(
    MpActor& actor, const std::function<void()>& execute, bool durable,
    std::optional<uint64_t> jobId = std::nullopt,
    const std::vector<uint64_t>* admissions = nullptr);
};
