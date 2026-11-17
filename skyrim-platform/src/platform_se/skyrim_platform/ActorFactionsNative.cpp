#include "ActorFactionsNative.h"
#include "PapyrusTESModPlatform.h"
#include <RE/E/ExtraFactionChanges.h>

namespace {
using namespace ActorFactions;

RE::TESFaction* Faction(uint32_t id)
{
  auto result = RE::TESForm::LookupByID<RE::TESFaction>(id);
  if (!result || result->IsDeleted())
    throw std::runtime_error("Native faction is missing or deleted");
  return result;
}

// Caller holds the actor's extra-list lock. Reject duplicate records rather
// than choosing a layer which another engine query might not choose.
RE::ExtraFactionChanges* Extra(RE::Actor& actor)
{
  RE::ExtraFactionChanges* result = nullptr;
  for (auto& data : actor.extraList) {
    if (data.GetType() != RE::ExtraDataType::kFactionChanges)
      continue;
    if (result)
      throw std::runtime_error("Duplicate native faction extra record");
    result = static_cast<RE::ExtraFactionChanges*>(&data);
  }
  return result;
}

struct PlayerFactionEvent
{
  RE::Actor* actor;
  RE::TESFaction* faction;
  int8_t rank;
  uint8_t padding[7]{};
};
static_assert(sizeof(PlayerFactionEvent) == 0x18);

void Notify(RE::Actor& actor, const std::vector<PlayerFactionEvent>& events)
{
  // Audited against Skyrim AE 1.7.104. These are the notification tail of
  // SetFactionRank, without its unique-NPC branch that edits a shared base.
  // The actual unique-base branch jumps past the player event dispatch. Do
  // not synthesize story events that the corresponding native setter omits.
  if (&actor == RE::PlayerCharacter::GetSingleton() &&
      !actor.GetActorBase()->IsUnique()) {
    REL::Relocation<void* (*)()> source{ REL::ID(38341) };
    REL::Relocation<void (*)(void*, const PlayerFactionEvent*)> dispatch{
      REL::ID(38316)
    };
    for (const auto& event : events)
      dispatch(source(), &event);
  }
  REL::Relocation<void (*)(RE::Actor*, bool)> invalidateRelations{ REL::ID(
    37668) };
  REL::Relocation<void (*)(RE::Actor*)> invalidateCrimeCombat{ REL::ID(
    37917) };
  invalidateRelations(&actor, true);
  invalidateCrimeCombat(&actor);
  if (auto process = actor.GetActorRuntimeData().currentProcess)
    if (process->cachedValues)
      process->cachedValues->flags.reset(
        RE::CachedValues::Flags::kOwnerIsInCombatantFaction);
}
}

bool ActorFactionsNative::SupportedRuntime()
{
  return REL::Module::IsAE() &&
    REL::Module::get().version() == REL::Version(1, 7, 104, 0);
}

ActorFactionsNative::Identity ActorFactionsNative::Bind(uint32_t actorId)
{
  if (!SupportedRuntime())
    throw std::runtime_error("Actor factions require audited Skyrim 1.7.104");
  if (actorId != 0x14 && actorId < 0xff000000)
    throw std::runtime_error(
      "Actor factions cannot edit an authored reference");
  auto actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
  if (!actor || actor->IsDeleted() || !actor->GetActorBase() ||
      actor->GetActorBase()->IsDeleted() ||
      (actorId == 0x14 && actor != RE::PlayerCharacter::GetSingleton()))
    throw std::runtime_error("Actor faction lease target is unavailable");
  auto handle = actor->GetHandle();
  if (!handle || handle.get().get() != actor)
    throw std::runtime_error("Actor faction lease has no stable handle");
  return { actorId, actor->GetActorBase()->GetFormID(), handle };
}

RE::NiPointer<RE::Actor> ActorFactionsNative::Resolve(const Identity& identity)
{
  auto actor = identity.handle.get();
  auto registered = RE::TESForm::LookupByID<RE::Actor>(identity.actorId);
  if (!actor) {
    if (registered && !registered->IsDeleted())
      throw std::runtime_error(
        "Actor faction handle was lost while its form remains live");
    return {};
  }
  if (actor->IsDeleted() || actor->GetFormID() != identity.actorId ||
      registered != actor.get())
    return {};
  if (!actor->GetActorBase() || actor->GetActorBase()->IsDeleted() ||
      actor->GetActorBase()->GetFormID() != identity.baseId)
    throw std::runtime_error("Actor faction lease base identity changed");
  return actor;
}

ActorFactionsNative::Snapshot ActorFactionsNative::Read(
  const Identity& identity, const Target& candidates)
{
  if (!SupportedRuntime())
    throw std::runtime_error("Actor faction runtime is unsupported");
  auto actor = Resolve(identity);
  if (!actor)
    throw std::runtime_error("Actor faction lease has expired");
  Snapshot result;
  result.identity = identity;
  result.player = actor.get() == RE::PlayerCharacter::GetSingleton();
  result.update = TESModPlatform::GetNumPapyrusUpdates();
  auto base = actor->GetActorBase();
  result.unique = base->IsUnique();
  if (base->factions.size() > kMaxEntries)
    throw std::runtime_error("Native base factions exceed bridge limit");
  for (const auto& row : base->factions) {
    if (!row.faction || row.faction->IsDeleted())
      throw std::runtime_error("Native base faction is unavailable");
    result.base.push_back(
      { row.faction->GetFormID(), row.rank,
        (static_cast<uint32_t>(row.faction->data.flags) & 0x4u) != 0 });
  }
  Validate(result.base);
  {
    RE::BSReadLockGuard lock(actor->extraList.GetLock());
    if (auto extra = Extra(*actor)) {
      result.hadExtra = true;
      if (extra->factionChanges.size() > kMaxEntries)
        throw std::runtime_error(
          "Native faction overrides exceed bridge limit");
      for (const auto& row : extra->factionChanges) {
        if (!row.faction || row.faction->IsDeleted())
          throw std::runtime_error("Native faction override is unavailable");
        result.rows.push_back({ row.faction->GetFormID(), row.rank });
      }
    }
  }
  Validate(result.rows);
  for (auto id : Candidates(result.base, result.rows, candidates)) {
    auto faction = Faction(id);
    result.observed.push_back({ id,
                                actor->GetFactionRank(faction, result.player),
                                actor->IsInFaction(faction) });
  }
  if (result.update != TESModPlatform::GetNumPapyrusUpdates())
    throw std::runtime_error("Faction observation crossed a Papyrus update");
  ValidateSnapshot(result);
  return result;
}

void ActorFactionsNative::ValidateSnapshot(const Snapshot& snapshot)
{
  const Layer layer(snapshot.base, snapshot.rows, snapshot.player);
  for (const auto& row : snapshot.observed)
    if (row != layer.Get(row.factionId))
      throw std::runtime_error(
        "Native faction queries require an unsupported actor/base layer");
}

void ActorFactionsNative::Write(const Snapshot& before, const Rows& rows,
                                bool requireExtra,
                                const std::function<void()>& arm)
{
  Validate(rows);
  auto actor = Resolve(before.identity);
  if (!actor)
    throw std::runtime_error("Actor disappeared before faction publication");
  RE::BSTArray<RE::FACTION_RANK> prepared(static_cast<uint32_t>(rows.size()));
  Target candidates;
  for (size_t i = 0; i < rows.size(); ++i) {
    prepared[static_cast<uint32_t>(i)] =
      RE::FACTION_RANK{ Faction(rows[i].factionId),
                        static_cast<int8_t>(rows[i].rank) };
    candidates.push_back({ rows[i].factionId, rows[i].rank, false });
  }
  const auto fresh = Read(before.identity, candidates);
  if (fresh.base != before.base || fresh.unique != before.unique ||
      fresh.rows != before.rows || fresh.hadExtra != before.hadExtra ||
      fresh.update != before.update)
    throw std::runtime_error("Actor faction state changed before publication");
  std::vector<PlayerFactionEvent> events;
  const Layer previous(before.base, before.rows, before.player);
  const Layer nextLayer(before.base, rows, before.player);
  for (auto id : Candidates(before.base, before.rows, candidates)) {
    const auto old = previous.Get(id);
    const auto next = nextLayer.Get(id);
    if (old != next)
      events.push_back(
        { actor.get(), Faction(id), static_cast<int8_t>(next.rank) });
  }
  bool removeExtra = false;
  {
    RE::BSReadLockGuard lock(actor->extraList.GetLock());
    const auto extra = Extra(*actor);
    removeExtra = !requireExtra && rows.empty() && extra &&
      !extra->crimeFaction && !extra->removeCrimeFaction;
  }
  if (rows == before.rows && (!requireExtra || before.hadExtra) &&
      !removeExtra)
    return;
  // From here, every failure retains the native lease and uncertainty barrier.
  arm();
  actor->AddChange(0x80000000u);
  if (!before.hadExtra && (requireExtra || !rows.empty())) {
    REL::Relocation<void (*)(RE::ExtraDataList*)> ensure{ REL::ID(12007) };
    ensure(&actor->extraList);
  }
  RE::ExtraFactionChanges* removable = nullptr;
  {
    RE::BSWriteLockGuard lock(actor->extraList.GetLock());
    auto extra = Extra(*actor);
    if (!extra) {
      if (requireExtra || !rows.empty())
        throw std::runtime_error("Native faction record allocation failed");
    } else {
      extra->factionChanges = std::move(prepared);
      // Crime overrides can change independently during the lease. Never
      // restore their old values or discard a record that now contains them.
      if (!requireExtra && rows.empty() && !extra->crimeFaction &&
          !extra->removeCrimeFaction)
        removable = extra;
    }
  }
  // All publication runs on the game thread; no engine callbacks intervene
  // between the crime-field check above and this lock-taking list removal.
  if (removable) {
    if (!actor->extraList.Remove(removable))
      throw std::runtime_error(
        "Could not remove restored faction extra record");
    delete removable;
  }
  Notify(*actor, events);
}
