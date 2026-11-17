#pragma once
#include "Faction.h"
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>

class MpActor;
class MpObjectReference;
class Inventory;
class MpChangeFormREFR;

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
};
