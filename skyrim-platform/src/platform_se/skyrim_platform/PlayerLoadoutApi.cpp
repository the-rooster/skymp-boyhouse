#include "PlayerLoadoutApi.h"
#include "CallNativeApi.h"
#include "NapiHelper.h"
#include "PapyrusTESModPlatform.h"
#include "PlayerLoadoutNative.h"
#include "SkyrimPlatform.h"
#include <chrono>
#include <mutex>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
using namespace PlayerLoadout;
using Clock = std::chrono::steady_clock;
constexpr auto kEffectDeadline = std::chrono::seconds(5);

struct Job
{
  std::string epoch, status = "idle", error;
  std::optional<uint64_t> revision;
  Target requested, target;
  std::map<uint32_t, BaseInfo> validatedBases;
  std::map<uint32_t, SpellInfo> validatedSpellBases;
  SettlementEvidence evidence;
  EligibleBudget budget;
};
std::mutex mutex;
std::shared_ptr<Job> current;
uint64_t worldGeneration = 1, epochCounter = 0;
uint64_t runtimeTicket = 1;
bool loading = false;
PendingOperation pending;
Clock::time_point effectDeadline;
bool pumpScheduled = false;

bool Active(const Job& job)
{
  return NeedsWork(job.revision.has_value(), job.status, false, false);
}

Napi::Object Object(Napi::Value value, const char* name)
{
  if (!value.IsObject() || value.IsArray() || value.IsNull())
    throw std::runtime_error(std::string("Invalid loadout ") + name);
  return value.As<Napi::Object>();
}
void Keys(Napi::Object object, std::initializer_list<const char*> allowed)
{
  const auto keys = object.GetPropertyNames();
  for (uint32_t i = 0; i < keys.Length(); ++i) {
    const auto key = keys.Get(i).As<Napi::String>().Utf8Value();
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](auto item) { return key == item; }))
      throw std::runtime_error("Unsupported loadout field: " + key);
  }
}
bool Present(Napi::Object object, const char* key)
{
  return object.HasOwnProperty(key) && !object.Get(key).IsUndefined();
}
uint64_t Integer(Napi::Value value, uint64_t max, const char* name,
                 uint64_t min = 0)
{
  if (!value.IsNumber())
    throw std::runtime_error(std::string("Invalid loadout ") + name);
  const auto number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number != std::floor(number) ||
      number < double(min) || number > double(max))
    throw std::runtime_error(std::string("Invalid loadout ") + name);
  return static_cast<uint64_t>(number);
}
float Float(Napi::Value value, const char* name)
{
  if (!value.IsNumber())
    throw std::runtime_error(std::string("Invalid loadout ") + name);
  const auto number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number < 0 ||
      number > std::numeric_limits<float>::max())
    throw std::runtime_error(std::string("Invalid loadout ") + name);
  const auto result = static_cast<float>(number);
  if (number != 0 && result == 0)
    throw std::runtime_error("Loadout metadata underflows float32");
  return result;
}
bool Boolean(Napi::Object object, const char* key)
{
  if (!Present(object, key))
    return false;
  const auto value = object.Get(key);
  if (!value.IsBoolean())
    throw std::runtime_error(std::string("Invalid loadout ") + key);
  return value.As<Napi::Boolean>().Value();
}
Item ReadItem(Napi::Object object)
{
  Keys(object,
       { "baseId", "count", "health", "chargePercent", "name", "enchantmentId",
         "maxCharge", "removeEnchantmentOnUnequip", "soul", "poisonId",
         "poisonCount", "worn", "wornLeft" });
  Item item;
  item.baseId = static_cast<uint32_t>(
    Integer(object.Get("baseId"), UINT32_MAX, "baseId", 1));
  item.count =
    static_cast<uint32_t>(Integer(object.Get("count"), kMaxCount, "count", 1));
  auto& x = item.extras;
  if (Present(object, "health"))
    x.health = Float(object.Get("health"), "health");
  if (Present(object, "chargePercent"))
    x.charge = Float(object.Get("chargePercent"), "chargePercent");
  if (Present(object, "name")) {
    if (!object.Get("name").IsString())
      throw std::runtime_error("Invalid loadout name");
    auto name = object.Get("name").As<Napi::String>().Utf8Value();
    if (name.size() > kMaxNameBytes || name.find('\0') != std::string::npos)
      throw std::runtime_error("Loadout name exceeds bridge support");
    x.name = std::move(name);
  }
  if (Present(object, "enchantmentId"))
    x.enchantmentId = static_cast<uint32_t>(
      Integer(object.Get("enchantmentId"), UINT32_MAX, "enchantmentId", 1));
  if (Present(object, "maxCharge"))
    x.maxCharge = static_cast<uint32_t>(
      Integer(object.Get("maxCharge"), 65535, "maxCharge"));
  if (Present(object, "removeEnchantmentOnUnequip"))
    x.removeEnchantmentOnUnequip =
      Boolean(object, "removeEnchantmentOnUnequip");
  if (Present(object, "soul"))
    x.soul = static_cast<uint32_t>(Integer(object.Get("soul"), 5, "soul"));
  if (Present(object, "poisonId"))
    x.poisonId = static_cast<uint32_t>(
      Integer(object.Get("poisonId"), UINT32_MAX, "poisonId", 1));
  if (Present(object, "poisonCount"))
    x.poisonCount = static_cast<uint32_t>(
      Integer(object.Get("poisonCount"), kMaxCount, "poisonCount"));
  Boolean(object, "worn");
  Boolean(object, "wornLeft");
  return item;
}
template <class F>
void Entries(Napi::Value value, F callback)
{
  auto object = Object(value, "inventory");
  Keys(object, { "entries" });
  const auto entries = object.Get("entries");
  if (!entries.IsArray())
    throw std::runtime_error("Loadout entries must be an array");
  const auto array = entries.As<Napi::Array>();
  if (array.Length() > kMaxStacks)
    throw std::runtime_error("Too many loadout entries");
  for (uint32_t i = 0; i < array.Length(); ++i) {
    if (!array.HasOwnProperty(std::to_string(i)))
      throw std::runtime_error("Sparse loadout entries are unsupported");
    auto entry = Object(array.Get(i), "entry");
    callback(ReadItem(entry), Boolean(entry, "worn"),
             Boolean(entry, "wornLeft"));
  }
}
Target ParseTarget(Napi::Object input)
{
  Keys(input, { "revision", "inventory", "equipment" });
  Target result;
  if (Present(input, "inventory")) {
    result.inventory.emplace();
    Entries(input.Get("inventory"), [&](Item item, bool, bool) {
      result.inventory->push_back(std::move(item));
    });
  }
  if (Present(input, "equipment")) {
    auto equipment = Object(input.Get("equipment"), "equipment");
    Keys(equipment,
         { "inv", "leftSpell", "rightSpell", "voiceSpell", "instantSpell" });
    result.equipment.emplace();
    Entries(equipment.Get("inv"), [&](Item item, bool right, bool left) {
      if (right && left && item.count < 2)
        throw std::runtime_error(
          "Dual-wield loadout requires two item copies");
      if (right || left)
        result.equipment->items.push_back(
          { item.baseId, std::move(item.extras), right, left });
    });
    const char* names[] = { "leftSpell", "rightSpell", "voiceSpell",
                            "instantSpell" };
    for (uint32_t slot = 0; slot < 4; ++slot)
      if (Present(equipment, names[slot]))
        result.equipment->spells[slot] = static_cast<uint32_t>(
          Integer(equipment.Get(names[slot]), UINT32_MAX, names[slot]));
  }
  // Input-only normalization permits deterministic deduplication even when a
  // request arrives on a tick. Real base names/slot masks are checked on the
  // game thread before any effect, and retained for subsequent duplicate
  // checks.
  return CanonicalInputTarget(result);
}
std::string Epoch(Napi::Value value)
{
  if (!value.IsString())
    throw std::runtime_error("Invalid loadout epoch");
  auto epoch = value.As<Napi::String>().Utf8Value();
  if (epoch.empty() || epoch.size() > 128)
    throw std::runtime_error("Invalid loadout epoch");
  return epoch;
}

void Fail(const std::shared_ptr<Job>& job, const std::string& error)
{
  if (job && current == job && Active(*job)) {
    job->status = "failed";
    job->error = error;
  }
}
void Deadlines()
{
  const auto now = Clock::now();
  if (pending.HasPending() && now >= effectDeadline)
    pending.Quarantine(
      "Native loadout effect timed out; a world reload is required");
  if (pending.IsQuarantined())
    Fail(current, pending.Error());
}
Napi::Object Status(Napi::Env env, const std::string& epoch)
{
  auto output = Napi::Object::New(env);
  std::lock_guard lock(mutex);
  Deadlines();
  output.Set("epoch", epoch);
  output.Set("status",
             current && current->epoch == epoch ? current->status
                                                : "superseded");
  if (current && current->epoch == epoch) {
    if (current->revision)
      output.Set("revision", double(*current->revision));
    if (!current->error.empty())
      output.Set("error", current->error);
  }
  output.Set("mutationPending", pending.HasPending() || loading);
  output.Set("uncertain", pending.IsQuarantined());
  if (pending.IsQuarantined() && !output.HasOwnProperty("error"))
    output.Set("error", pending.Error());
  return output;
}

Napi::Value Native(Napi::Env env, const char* type, const char* method,
                   Napi::Value self,
                   std::initializer_list<Napi::Value> parameters = {})
{
  std::vector<Napi::Value> args{ Napi::String::New(env, type),
                                 Napi::String::New(env, method), self };
  args.insert(args.end(), parameters.begin(), parameters.end());
  return CallNativeApi::CallNative(env, args,
                                   [] { return g_nativeCallRequirements; });
}
struct Spells
{
  Napi::Value player;
  std::array<uint32_t, 4> slots{};
  uint64_t update;
};
uint32_t FormId(Napi::Env env, Napi::Value form)
{
  if (form.IsNull() || form.IsUndefined())
    return 0;
  // Form.GetFormID is a signed Papyrus int; preserve its full bit pattern.
  auto id = Native(env, "Form", "GetFormID", form);
  if (!id.IsNumber())
    throw std::runtime_error("Native loadout form ID is unavailable");
  return static_cast<uint32_t>(id.As<Napi::Number>().Int32Value());
}
Spells ReadSpells(Napi::Env env)
{
  Spells result{ Native(env, "Game", "GetPlayer", env.Null()),
                 {},
                 TESModPlatform::GetNumPapyrusUpdates() };
  if (FormId(env, result.player) != 0x14)
    throw std::runtime_error("Native loadout player is unavailable");
  for (uint32_t slot = 0; slot < 4; ++slot)
    result.slots[slot] =
      FormId(env,
             Native(env, "Actor", "GetEquippedSpell", result.player,
                    { Napi::Number::New(env, slot) }));
  if (result.update != TESModPlatform::GetNumPapyrusUpdates())
    throw std::runtime_error("Native loadout spell snapshot expired");
  return result;
}

struct SpellAction
{
  std::shared_ptr<Job> job;
  uint64_t generation, revision;
  Operation operation;
  Snapshot before;
};
void Pump(Napi::Env env, uint64_t ticket);
void Schedule()
{
  uint64_t ticket;
  {
    std::lock_guard lock(mutex);
    if (pumpScheduled || loading ||
        !NeedsWork(current && current->revision.has_value(),
                   current ? current->status : "idle", pending.HasPending(),
                   pending.IsQuarantined()))
      return;
    pumpScheduled = true;
    ticket = runtimeTicket;
  }
  SkyrimPlatform::GetSingleton()->AddUpdateTask(
    [ticket](Napi::Env env) { Pump(env, ticket); });
}
bool Same(const std::shared_ptr<Job>& job, uint64_t revision,
          uint64_t generation)
{
  return !loading && generation == worldGeneration && current == job && job &&
    job->revision == revision && Active(*job);
}

void Pump(Napi::Env env, uint64_t ticket)
{
  std::shared_ptr<Job> job;
  Target target;
  uint64_t generation, revision = 0;
  {
    std::lock_guard lock(mutex);
    if (ticket != runtimeTicket)
      return;
    pumpScheduled = false;
    Deadlines();
    if (loading ||
        !NeedsWork(current && current->revision.has_value(),
                   current ? current->status : "idle", pending.HasPending(),
                   pending.IsQuarantined()))
      return;
    generation = worldGeneration;
    if (current && Active(*current)) {
      job = current;
      revision = *job->revision;
      target = job->target;
    }
  }
  try {
    bool readable = false;
    SkyrimPlatform::GetSingleton()->RunOnGameThreadAndWait(
      [&] { readable = PlayerLoadoutNative::Readable(); });
    if (!readable) {
      {
        std::lock_guard lock(mutex);
        if (Same(job, revision, generation))
          job->budget.Observe(
            std::chrono::duration_cast<std::chrono::milliseconds>(
              Clock::now().time_since_epoch())
              .count(),
            false);
      }
      Schedule();
      return;
    }
    const auto spells = ReadSpells(env);
    std::optional<SpellAction> spellAction;
    SkyrimPlatform::GetSingleton()->RunOnGameThreadAndWait([&] {
      if (!PlayerLoadoutNative::Readable())
        return;
      bool observing;
      {
        std::lock_guard lock(mutex);
        if (loading || generation != worldGeneration)
          return;
        observing = pending.HasPending();
      }
      auto snapshot =
        PlayerLoadoutNative::Read(observing ? Target{} : target, spells.slots);
      if (spells.update != TESModPlatform::GetNumPapyrusUpdates())
        throw std::runtime_error("Native loadout snapshot crossed an update");
      {
        std::lock_guard lock(mutex);
        if (loading || generation != worldGeneration)
          return;
        if (pending.HasPending()) {
          pending.ObserveFresh(snapshot.value, generation);
          Deadlines();
          return; // A later update replans after observing the previous
                  // effect.
        }
        if (!Same(job, revision, generation))
          return;
      }
      const auto canonical = CanonicalTarget(snapshot.value, target);
      const auto plan = BuildPlan(snapshot.value, canonical);
      PlayerLoadoutNative::Preflight(snapshot, canonical, plan);
      const bool ready = PlayerLoadoutNative::Ready();
      std::optional<Operation> dispatch;
      {
        std::lock_guard lock(mutex);
        if (!Same(job, revision, generation) || pending.HasPending() ||
            pending.IsQuarantined())
          return;
        job->target = canonical;
        job->validatedBases = snapshot.value.bases;
        job->validatedSpellBases = snapshot.value.spellBases;
        if (job->budget.Observe(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch())
                .count(),
              ready)) {
          Fail(job, "Native loadout exceeded eligible restoration time");
          return;
        }
        if (plan.matches) {
          job->status = job->evidence.ObserveMatch(snapshot.value.stamp, true)
            ? "settled"
            : "observing";
          return;
        }
        job->evidence.ObserveMatch(snapshot.value.stamp, false);
        job->status = "queued";
        if (!ready)
          return;
        const auto operation = BoundOperation(plan.operations.front(), 32767);
        if (operation.kind == OperationKind::Spell) {
          spellAction = SpellAction{ job, generation, revision, operation,
                                     snapshot.value };
          job->status = "working";
          return;
        }
        pending.Arm(operation, snapshot.value, generation);
        pending.MarkDispatched();
        effectDeadline = Clock::now() + kEffectDeadline;
        job->status = "observing";
        dispatch = operation;
      }
      // Claim precedes callbacks; cancellation cannot erase this outstanding
      // effect. No raw list binding survives this one synchronous operation.
      PlayerLoadoutNative::Apply(snapshot, *dispatch);
    });
    if (spellAction) {
      const auto fresh = ReadSpells(env);
      auto& action = *spellAction;
      const auto id = action.operation.spellId
        ? action.operation.spellId
        : fresh.slots[action.operation.spellSlot];
      auto form = Native(env, "Game", "GetForm", env.Null(),
                         { Napi::Number::New(env, static_cast<int32_t>(id)) });
      if (!id || FormId(env, form) != id)
        throw std::runtime_error("Loadout spell disappeared before dispatch");
      std::optional<Snapshot> spellBefore;
      Operation spellOperation;
      SkyrimPlatform::GetSingleton()->RunOnGameThreadAndWait([&] {
        if (!PlayerLoadoutNative::Ready() ||
            fresh.update != TESModPlatform::GetNumPapyrusUpdates())
          return;
        auto snapshot = PlayerLoadoutNative::Read(target, fresh.slots);
        // Re-read all item and spell state immediately before claiming the
        // builtin call. Neither a prior frame nor a stale plan authorizes it.
        if (Observe(action.operation, action.before, snapshot.value) !=
            Observation::Pending)
          return;
        auto plan = BuildPlan(snapshot.value, target);
        PlayerLoadoutNative::Preflight(snapshot, target, plan);
        if (plan.operations.empty() ||
            plan.operations.front().kind != OperationKind::Spell ||
            plan.operations.front().spellSlot != action.operation.spellSlot ||
            plan.operations.front().spellId != action.operation.spellId)
          return;
        std::lock_guard lock(mutex);
        if (!Same(action.job, action.revision, action.generation) ||
            pending.HasPending() || pending.IsQuarantined())
          return;
        spellOperation = plan.operations.front();
        spellBefore = snapshot.value;
      });
      bool dispatchSpell = false;
      if (spellBefore) {
        std::lock_guard lock(mutex);
        // Waiting dispatch is still cancelable. Claim only immediately before
        // the builtin call, after the same-update game-thread revalidation.
        if (Same(action.job, action.revision, action.generation) &&
            fresh.update == TESModPlatform::GetNumPapyrusUpdates() &&
            !pending.HasPending() && !pending.IsQuarantined()) {
          pending.Arm(spellOperation, *spellBefore, generation);
          pending.MarkDispatched();
          effectDeadline = Clock::now() + kEffectDeadline;
          action.job->status = "observing";
          dispatchSpell = true;
        }
      }
      if (dispatchSpell) {
        Native(env, "Actor",
               action.operation.spellId ? "EquipSpell" : "UnequipSpell",
               fresh.player,
               { form, Napi::Number::New(env, action.operation.spellSlot) });
      }
    }
  } catch (const std::exception& error) {
    std::lock_guard lock(mutex);
    if (generation == worldGeneration) {
      if (pending.HasPending())
        pending.Quarantine(
          std::string("Native loadout effect is uncertain: ") + error.what());
      if (Same(job, revision, generation))
        Fail(job, error.what());
      Deadlines();
    }
  } catch (...) {
    std::lock_guard lock(mutex);
    if (generation == worldGeneration) {
      if (pending.HasPending())
        pending.Quarantine(
          "Native loadout effect failed with an unknown exception");
      if (Same(job, revision, generation))
        Fail(job, "Native loadout failed with an unknown exception");
      Deadlines();
    }
  }
  Schedule();
}

Napi::Value Begin(const Napi::CallbackInfo& info)
{
  auto job = std::make_shared<Job>();
  {
    std::lock_guard lock(mutex);
    job->epoch = "player-loadout:" + std::to_string(worldGeneration) + ":" +
      std::to_string(++epochCounter);
    current = job;
  }
  Schedule();
  return Napi::String::New(info.Env(), job->epoch);
}
Napi::Value Request(const Napi::CallbackInfo& info)
{
  const auto epoch = Epoch(info[0]);
  auto input = Object(info[1], "request");
  const auto revision =
    Integer(input.Get("revision"), kMaxRevision, "revision", 1);
  auto target = ParseTarget(input);
  {
    std::lock_guard lock(mutex);
    Deadlines();
    if (current && current->epoch == epoch &&
        current->status != "superseded") {
      auto comparable = target;
      auto previous = current->requested;
      if (current->revision == revision && !(target == previous) &&
          !current->validatedBases.empty()) {
        Snapshot known;
        known.bases = current->validatedBases;
        known.spellBases = current->validatedSpellBases;
        comparable = CanonicalTarget(known, target);
        previous = CanonicalTarget(known, previous);
      }
      if (CheckRevision(current->revision, &previous, revision, comparable) ==
          RevisionDecision::Accept) {
        current->revision = revision;
        current->requested = target;
        current->target = std::move(target);
        current->validatedBases.clear();
        current->validatedSpellBases.clear();
        current->status = "queued";
        current->error.clear();
        current->evidence.Reset();
        current->budget.Reset();
        Deadlines();
      }
    }
  }
  Schedule();
  return Status(info.Env(), epoch);
}
Napi::Value GetStatus(const Napi::CallbackInfo& info)
{
  const auto epoch = Epoch(info[0]);
  Schedule();
  return Status(info.Env(), epoch);
}
Napi::Value Cancel(const Napi::CallbackInfo& info)
{
  const auto epoch = Epoch(info[0]);
  {
    std::lock_guard lock(mutex);
    if (current && current->epoch == epoch)
      current->status = "superseded";
  }
  Schedule();
  return info.Env().Undefined();
}
}

void PlayerLoadoutApi::InvalidateWorld()
{
  std::lock_guard lock(mutex);
  ++worldGeneration;
  pending.InvalidateWorld(worldGeneration);
  loading = false;
  if (current)
    current->status = "superseded";
}

void PlayerLoadoutApi::BeginLoad()
{
  std::lock_guard lock(mutex);
  loading = true;
  if (current)
    current->status = "superseded";
  if (pending.HasPending())
    pending.Quarantine(
      "World load interrupted an unobserved native loadout effect");
}

void PlayerLoadoutApi::CompleteLoad(bool success)
{
  std::lock_guard lock(mutex);
  if (success) {
    ++worldGeneration;
    pending.InvalidateWorld(worldGeneration);
  }
  // Failure resumes the old world, retaining its unresolved/quarantined
  // effect.
  loading = false;
  if (current)
    current->status = "superseded";
}

void PlayerLoadoutApi::ResetRuntime()
{
  {
    std::lock_guard lock(mutex);
    ++runtimeTicket;
    pumpScheduled = false;
    if (current)
      current->status = "superseded";
  }
  // A discarded JS task cannot strand an already dispatched engine effect.
  // Old callbacks carry the prior ticket and become harmless if still queued.
  Schedule();
}

void PlayerLoadoutApi::Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("playerLoadoutApiVersion", Napi::Number::New(env, 1));
  exports.Set("beginPlayerLoadout",
              Napi::Function::New(env, NapiHelper::WrapCppExceptions(Begin)));
  exports.Set(
    "requestPlayerLoadout",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(Request)));
  exports.Set(
    "getPlayerLoadoutStatus",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetStatus)));
  exports.Set("cancelPlayerLoadout",
              Napi::Function::New(env, NapiHelper::WrapCppExceptions(Cancel)));
}
