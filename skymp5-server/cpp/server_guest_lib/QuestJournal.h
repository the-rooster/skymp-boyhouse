#pragma once
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>

class MpActor;
class MpObjectReference;
class Inventory;

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
  // Synchronous journal/VM and staged-inventory transaction. Other world
  // effects and latent natives still require transactional adapters.
  static void Atomic(MpActor& actor, const std::function<void()>& execute);
  static bool HasActiveTransaction() noexcept;
  static const Inventory& ReadInventory(MpActor& actor,
                                        MpObjectReference& target);
  static void EditInventory(MpActor& actor, MpObjectReference& target,
                            const std::function<void(Inventory&)>& edit);
  static nlohmann::json ScriptState(MpActor& actor);
  static void SetScriptState(MpActor& actor, const nlohmann::json& state);
  static bool ReferenceInitialized(MpActor& actor, uint32_t original);
  static void MarkReferenceInitialized(MpActor& actor, uint32_t original);
};
