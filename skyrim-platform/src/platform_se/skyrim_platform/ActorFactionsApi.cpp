#include "ActorFactionsApi.h"
#include "ActorFactionsNative.h"
#include "NapiHelper.h"
#include "PapyrusTESModPlatform.h"
#include "SkyrimPlatform.h"
#include <cmath>
#include <mutex>

namespace {
using namespace ActorFactions;
struct Attempt
{
  Rows rows;
  Target target;
  uint64_t revision = 0;
  bool restoring = false, requireExtra = false;
  Observations observations;
};
struct Lease
{
  std::string token, status = "idle", error;
  uint64_t generation = 0;
  ActorFactionsNative::Snapshot original;
  std::optional<uint64_t> revision;
  Target requested;
  std::shared_ptr<Attempt> attempt;
  bool restoreRequested = false, uncertain = false;
  bool validateRequested = false;
  uint64_t verifiedUpdate = 0;
};
std::mutex mutex;
std::map<std::string, std::shared_ptr<Lease>> leases;
uint64_t worldGeneration = 1, nextLease = 0, runtimeTicket = 1;
std::string lastPumpedToken;
bool loading = false, scheduled = false;
constexpr size_t kMaxLeases = 256;

bool Live(const Lease& lease)
{
  return lease.status != "superseded" && lease.status != "restored";
}
bool Active(const Lease& lease)
{
  return lease.status == "queued" || lease.status == "observing" ||
    lease.status == "restoring";
}
bool NeedsWork(const Lease& lease)
{
  return Active(lease) ||
    (lease.status == "settled" && lease.validateRequested);
}
uint64_t Integer(Napi::Value value, uint64_t min, uint64_t max,
                 const char* name)
{
  if (!value.IsNumber())
    throw std::runtime_error(std::string("Invalid actor faction ") + name);
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < double(min) || number > double(max))
    throw std::runtime_error(std::string("Invalid actor faction ") + name);
  return static_cast<uint64_t>(number);
}
std::string Token(Napi::Value value)
{
  if (!value.IsString())
    throw std::runtime_error("Invalid actor faction token");
  auto result = value.As<Napi::String>().Utf8Value();
  if (result.empty() || result.size() > 128)
    throw std::runtime_error("Invalid actor faction token");
  return result;
}
Target Parse(Napi::Value value)
{
  if (!value.IsArray())
    throw std::runtime_error("Actor factions must be an array");
  const auto array = value.As<Napi::Array>();
  if (array.Length() > kMaxEntries)
    throw std::runtime_error("Too many actor faction entries");
  Target result;
  for (uint32_t i = 0; i < array.Length(); ++i) {
    if (!array.HasOwnProperty(std::to_string(i)))
      throw std::runtime_error("Sparse actor factions are unsupported");
    const auto value = array.Get(i);
    if (!value.IsObject() || value.IsArray() || value.IsNull())
      throw std::runtime_error("Invalid actor faction entry");
    const auto row = value.As<Napi::Object>();
    const auto keys = row.GetPropertyNames();
    if (keys.Length() != 3 || !row.HasOwnProperty("factionId") ||
        !row.HasOwnProperty("rank") || !row.HasOwnProperty("isMember"))
      throw std::runtime_error("Unexpected actor faction fields");
    const auto id = Integer(row.Get("factionId"), 1, UINT32_MAX, "form ID");
    if (!row.Get("rank").IsNumber() || !row.Get("isMember").IsBoolean())
      throw std::runtime_error("Invalid actor faction rank or membership");
    const double rank = row.Get("rank").As<Napi::Number>().DoubleValue();
    if (!std::isfinite(rank) || std::floor(rank) != rank || rank < -128 ||
        rank > 127)
      throw std::runtime_error("Invalid actor faction rank");
    result.push_back({ static_cast<uint32_t>(id), static_cast<int>(rank),
                       row.Get("isMember").As<Napi::Boolean>().Value() });
  }
  return Canonical(std::move(result));
}
bool Current(const std::shared_ptr<Lease>& lease)
{
  const auto found = leases.find(lease->token);
  return found != leases.end() && found->second == lease &&
    lease->generation == worldGeneration && Live(*lease);
}
void Fail(const std::shared_ptr<Lease>& lease, const char* message,
          uint64_t ticket)
{
  std::lock_guard lock(mutex);
  if (ticket != runtimeTicket || !Current(lease))
    return;
  lease->status = "failed";
  lease->error = message;
  // The attempt and exact original rows remain available to restoration.
  // No new lease can capture the modified state as its original state.
}
Napi::Object Status(Napi::Env env, const std::string& token)
{
  auto result = Napi::Object::New(env);
  std::lock_guard lock(mutex);
  const auto found = leases.find(token);
  const auto lease = found == leases.end() ? nullptr : found->second;
  result.Set("token", token);
  result.Set("actorId", lease ? lease->original.identity.actorId : 0);
  result.Set("status", lease ? lease->status : "superseded");
  result.Set("mutationPending",
             lease && Live(*lease) &&
               (loading || Active(*lease) || lease->uncertain));
  // An armed synchronous publication normally waits for observation. Only a
  // failed attempt is indeterminate to the caller; normal observing/restoring
  // is represented by mutationPending and must remain pollable.
  result.Set("uncertain",
             lease && lease->status == "failed" && lease->uncertain);
  if (lease && lease->revision)
    result.Set("revision", double(*lease->revision));
  if (lease && !lease->error.empty())
    result.Set("error", lease->error);
  return result;
}

void Pump(uint64_t ticket);
void Schedule()
{
  uint64_t ticket;
  {
    std::lock_guard lock(mutex);
    if (loading || scheduled ||
        std::none_of(leases.begin(), leases.end(), [](const auto& entry) {
          return NeedsWork(*entry.second);
        }))
      return;
    scheduled = true;
    ticket = runtimeTicket;
  }
  SkyrimPlatform::GetSingleton()->AddUpdateTask(
    [ticket](Napi::Env) { Pump(ticket); });
}

void Process(const std::shared_ptr<Lease>& lease)
{
  Target requested;
  std::shared_ptr<Attempt> attempt;
  bool restoring;
  bool validateOnly;
  uint64_t revision = 0;
  {
    std::lock_guard lock(mutex);
    if (loading || !Current(lease) || !NeedsWork(*lease))
      return;
    requested = lease->requested;
    attempt = lease->attempt;
    restoring = lease->restoreRequested;
    validateOnly = lease->status == "settled";
    revision = lease->revision.value_or(0);
  }
  auto actor = ActorFactionsNative::Resolve(lease->original.identity);
  if (!actor) {
    std::lock_guard lock(mutex);
    if (Current(lease)) {
      // Its engine handle expired. Never seek a new actor by the reused ID.
      lease->status = "superseded";
      lease->attempt.reset();
      lease->uncertain = false;
    }
    return;
  }
  auto candidates = requested;
  candidates.insert(candidates.end(), lease->original.observed.begin(),
                    lease->original.observed.end());
  const auto snapshot =
    ActorFactionsNative::Read(lease->original.identity, candidates);
  // Application owns only the reference-local override layer. A changed
  // immutable base invalidates its prior plan, but must not prevent cleanup
  // from restoring original raw overrides against the CURRENT base/flags.
  if (!restoring &&
      (snapshot.base != lease->original.base ||
       snapshot.unique != lease->original.unique ||
       snapshot.player != lease->original.player))
    throw std::runtime_error(
      "Actor faction immutable base changed during lease");

  if (validateOnly) {
    if (!Matches(snapshot.base, snapshot.rows, snapshot.player, requested))
      throw std::runtime_error(
        "Settled actor faction state drifted from the server target");
    std::lock_guard lock(mutex);
    if (!loading && Current(lease) && lease->status == "settled" &&
        lease->revision == revision) {
      lease->validateRequested = false;
      lease->verifiedUpdate = snapshot.update;
    }
    return;
  }

  if (attempt && (!restoring || attempt->restoring)) {
    if (snapshot.rows != attempt->rows ||
        (attempt->requireExtra && !snapshot.hadExtra) ||
        (!attempt->restoring &&
         !Matches(snapshot.base, snapshot.rows, snapshot.player,
                  attempt->target)))
      throw std::runtime_error(
        "Native faction state drifted before observation");
    std::lock_guard lock(mutex);
    if (loading || !Current(lease) || lease->attempt != attempt)
      return;
    if (attempt->observations.Match(snapshot.update)) {
      lease->uncertain = false;
      lease->attempt.reset();
      lease->verifiedUpdate = snapshot.update;
      lease->validateRequested = false;
      lease->status = attempt->restoring       ? "restored"
        : lease->revision == attempt->revision ? "settled"
                                               : "queued";
    }
    return;
  }

  auto prepared = std::make_shared<Attempt>();
  prepared->restoring = restoring;
  prepared->revision = revision;
  prepared->target = requested;
  prepared->rows = restoring
    ? lease->original.rows
    : Plan(snapshot.base, snapshot.rows, snapshot.player, requested);
  prepared->requireExtra = restoring
    ? lease->original.hadExtra
    : snapshot.hadExtra || !prepared->rows.empty();
  prepared->observations.Reset(snapshot.update);
  {
    std::lock_guard lock(mutex);
    if (loading || !Current(lease) || lease->restoreRequested != restoring ||
        (!restoring && lease->revision != revision))
      return;
    lease->attempt = prepared;
    lease->status = restoring ? "restoring" : "observing";
  }
  // Publication is synchronous. An explicit restore may replace an earlier
  // observation attempt after reading the same handle/base afresh; no queued
  // native setter remains capable of applying the previous target later.
  ActorFactionsNative::Write(
    snapshot, prepared->rows, prepared->requireExtra, [&] {
      std::lock_guard lock(mutex);
      if (loading || !Current(lease) || lease->attempt != prepared)
        throw std::runtime_error(
          "Actor faction context expired before mutation");
      lease->uncertain = true;
    });
}

void Pump(uint64_t ticket)
{
  std::vector<std::shared_ptr<Lease>> work;
  {
    std::lock_guard lock(mutex);
    if (ticket != runtimeTicket)
      return;
    scheduled = false;
    if (loading)
      return;
    for (const auto& [token, lease] : leases)
      if (NeedsWork(*lease))
        work.push_back(lease);
    if (!work.empty()) {
      auto next =
        std::upper_bound(work.begin(), work.end(), lastPumpedToken,
                         [](const std::string& token, const auto& lease) {
                           return token < lease->token;
                         });
      if (next == work.end())
        next = work.begin();
      std::rotate(work.begin(), next, work.end());
      if (work.size() > 8)
        work.resize(8);
      lastPumpedToken = work.back()->token;
    }
  }
  for (const auto& lease : work) {
    {
      std::lock_guard lock(mutex);
      if (ticket != runtimeTicket || loading)
        break;
    }
    try {
      SkyrimPlatform::GetSingleton()->RunOnGameThreadAndWait(
        [&] { Process(lease); });
    } catch (const std::exception& error) {
      Fail(lease, error.what(), ticket);
    } catch (...) {
      Fail(lease, "Unknown native actor faction failure", ticket);
    }
  }
  Schedule();
}

Napi::Value Begin(const Napi::CallbackInfo& info)
{
  const auto id =
    static_cast<uint32_t>(Integer(info[0], 1, UINT32_MAX, "actor ID"));
  auto lease = std::make_shared<Lease>();
  {
    std::lock_guard lock(mutex);
    if (loading)
      throw std::runtime_error("Actor faction world is loading");
    for (auto it = leases.begin(); it != leases.end();) {
      if (it->second->original.identity.actorId != id) {
        ++it;
        continue;
      }
      if (Live(*it->second))
        throw std::runtime_error(
          "Actor's previous faction lease has not restored");
      it = leases.erase(it);
    }
    if (leases.size() >= kMaxLeases)
      throw std::runtime_error("Too many native actor faction leases");
    lease->generation = worldGeneration;
    lease->token = "actor-factions:" + std::to_string(worldGeneration) + ":" +
      std::to_string(++nextLease);
  }
  SkyrimPlatform::GetSingleton()->RunOnGameThreadAndWait([&] {
    lease->original = ActorFactionsNative::Read(ActorFactionsNative::Bind(id));
  });
  {
    std::lock_guard lock(mutex);
    if (loading || lease->generation != worldGeneration)
      throw std::runtime_error("Actor faction world changed during capture");
    leases.emplace(lease->token, lease);
  }
  return Napi::String::New(info.Env(), lease->token);
}
Napi::Value Apply(const Napi::CallbackInfo& info)
{
  const auto token = Token(info[0]);
  const auto revision = Integer(info[1], 1, kMaxRevision, "revision");
  auto target = Parse(info[2]);
  {
    std::lock_guard lock(mutex);
    const auto found = leases.find(token);
    if (found == leases.end())
      throw std::runtime_error("Unknown actor faction lease");
    const auto& lease = found->second;
    if (!Current(lease) || lease->restoreRequested ||
        lease->status == "failed")
      throw std::runtime_error(
        "Actor faction lease cannot accept another target");
    if (lease->revision == revision && lease->requested != target)
      throw std::runtime_error("Conflicting actor faction revision");
    if (!lease->revision || revision > *lease->revision) {
      lease->revision = revision;
      lease->requested = std::move(target);
      lease->status = "queued";
      lease->error.clear();
    }
  }
  Schedule();
  return Status(info.Env(), token);
}
Napi::Value Restore(const Napi::CallbackInfo& info)
{
  const auto token = Token(info[0]);
  {
    std::lock_guard lock(mutex);
    const auto found = leases.find(token);
    if (found != leases.end() && Current(found->second)) {
      const auto& lease = found->second;
      if (!lease->restoreRequested || lease->status == "failed") {
        lease->restoreRequested = true;
        lease->attempt.reset();
        lease->status = "restoring";
        lease->error.clear();
      }
    }
  }
  Schedule();
  return Status(info.Env(), token);
}
Napi::Value GetStatus(const Napi::CallbackInfo& info)
{
  const auto token = Token(info[0]);
  {
    std::lock_guard lock(mutex);
    const auto found = leases.find(token);
    const auto update = TESModPlatform::GetNumPapyrusUpdates();
    if (found != leases.end() && found->second->status == "settled" &&
        update >= found->second->verifiedUpdate &&
        update - found->second->verifiedUpdate >= 30)
      found->second->validateRequested = true;
  }
  Schedule();
  return Status(info.Env(), token);
}
Napi::Value Release(const Napi::CallbackInfo& info)
{
  const auto token = Token(info[0]);
  std::lock_guard lock(mutex);
  const auto found = leases.find(token);
  if (found != leases.end()) {
    if (Live(*found->second))
      throw std::runtime_error(
        "An unrestored actor faction lease cannot be released");
    leases.erase(found);
  }
  return info.Env().Undefined();
}
}

void ActorFactionsApi::BeginLoad()
{
  std::lock_guard lock(mutex);
  loading = true;
  ++runtimeTicket;
  scheduled = false;
  for (const auto& [token, lease] : leases) {
    if (!Live(*lease))
      continue;
    lease->restoreRequested = true;
    lease->attempt.reset();
    lease->status = "restoring";
  }
}
void ActorFactionsApi::InvalidateWorld()
{
  std::lock_guard lock(mutex);
  ++worldGeneration;
  ++runtimeTicket;
  loading = scheduled = false;
  for (const auto& [token, lease] : leases) {
    lease->status = "superseded";
    lease->attempt.reset();
    lease->uncertain = false;
  }
}
void ActorFactionsApi::CompleteLoad(bool success)
{
  if (success) {
    InvalidateWorld();
    return;
  }
  {
    std::lock_guard lock(mutex);
    loading = false;
  }
  Schedule();
}
void ActorFactionsApi::ResetRuntime()
{
  {
    std::lock_guard lock(mutex);
    ++runtimeTicket;
    scheduled = false;
    for (const auto& [token, lease] : leases) {
      if (!Live(*lease))
        continue;
      lease->restoreRequested = true;
      lease->attempt.reset();
      lease->status = "restoring";
    }
  }
  // Native originals/handles outlive JS modules, so a discarded task cannot
  // strand altered player state or let a new module capture it as original.
  Schedule();
}
void ActorFactionsApi::Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("actorFactionsApiVersion", Napi::Number::New(env, 1));
  exports.Set("beginActorFactions",
              Napi::Function::New(env, NapiHelper::WrapCppExceptions(Begin)));
  exports.Set("applyActorFactions",
              Napi::Function::New(env, NapiHelper::WrapCppExceptions(Apply)));
  exports.Set(
    "restoreActorFactions",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(Restore)));
  exports.Set(
    "getActorFactionsStatus",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetStatus)));
  exports.Set(
    "releaseActorFactions",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(Release)));
}
