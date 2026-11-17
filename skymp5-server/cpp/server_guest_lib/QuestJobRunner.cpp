#include "QuestJobRunner.h"
#include "MpActor.h"
#include "QuestJobContent.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "WorldState.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace {
const QuestJobState::Job* Find(const QuestJobState& state,
                               const std::string& command)
{
  if (state.active && state.active->command == command)
    return &*state.active;
  for (const auto& job : state.completed)
    if (job.command == command)
      return &job;
  return nullptr;
}

void RequireMode(const QuestJobState::Job& job, bool fixture)
{
  if (job.contentMode != "legacy" &&
      job.contentMode != (fixture ? "fixture" : "owned"))
    throw std::runtime_error("Quest job execution content mode changed");
}

espm::QuestScriptData Metadata(
  WorldState& world, uint32_t id,
  const QuestJobRunner::MetadataOverrides& overrides)
{
  const auto record = world.GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "QUST")
    throw std::invalid_argument("Expected a live quest record");
  if (const auto found = overrides.find(id); found != overrides.end())
    return found->second;
  return espm::QuestScriptData::Read(record.rec, world.GetEspmCache());
}

struct BusyGuard
{
  bool& busy;
  explicit BusyGuard(bool& value)
    : busy(value)
  {
    busy = true;
  }
  ~BusyGuard() { busy = false; }
};
}

QuestJobRunner::QuestJobRunner(WorldState& world)
  : world(world)
  , clock([] {
    const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    const auto steady = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    if (utc < 0 || steady < 0)
      throw std::runtime_error("Quest clock has a negative epoch");
    return QuestJobTime{ static_cast<uint64_t>(utc),
                         static_cast<uint64_t>(steady) };
  })
{
}

void QuestJobRunner::Stop() noexcept
{
  // Reusing a cleared world is not recovery: an abandoned write may still
  // commit after objects were removed. Reconstruct a server from storage to
  // recover jobs, after its previous writer has shut down.
  stopped = true;
  scheduled.clear();
}

QuestJobTime QuestJobRunner::Now()
{
  const auto time = clock();
  if (time.unixMs > 9007199254740991ULL ||
      (lastSteadyMs && time.steadyMs < *lastSteadyMs))
    throw std::runtime_error("Quest clock is invalid or moved backwards");
  lastSteadyMs = time.steadyMs;
  return time;
}

void QuestJobRunner::SetClock(QuestJobClock replacement)
{
  if (!replacement || stopped || pumping ||
      std::any_of(barriers.begin(), barriers.end(), [](const auto& entry) {
        return entry.second.busy || entry.second.timer.has_value();
      }))
    throw std::runtime_error("Cannot replace an armed quest clock");
  clock = std::move(replacement);
  lastSteadyMs.reset();
}

void QuestJobRunner::SetSchedulerFixtureContent(
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources)
{
  if (stopped || pumping || schedulerFixture)
    throw std::runtime_error(
      "Quest scheduler fixture content is already fixed");
  QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(sources), overrides);
  schedulerFixture = FixtureContent{ overrides, sources };
}

void QuestJobRunner::Track(MpActor& owner, const QuestJobState::Job& job)
{
  if (!job.automatic || stopped)
    return;
  if (job.owner != owner.GetChangeForm().formDesc.ToString())
    throw std::runtime_error(
      "Quest scheduling owner does not match saved work");
  if (!scheduled.contains(owner.GetFormId()) && scheduled.size() >= 4096)
    throw std::runtime_error("Quest scheduler campaign limit exceeded");
  const auto found = scheduled.find(owner.GetFormId());
  if (found != scheduled.end() && found->second.job == job.id &&
      found->second.command == job.command &&
      found->second.instance == job.instance &&
      found->second.contentMode == job.contentMode &&
      found->second.actor ==
        FormDesc::FromString(job.actor).ToFormId(world.espmFiles) &&
      found->second.owner.lock().get() == &owner)
    return;
  Scheduled entry;
  entry.job = job.id;
  entry.actor = FormDesc::FromString(job.actor).ToFormId(world.espmFiles);
  entry.command = job.command;
  entry.instance = job.instance;
  entry.contentMode = job.contentMode;
  entry.waitingExternal =
    job.Current().cursor == "waiting" && job.Current().awaitKind == "bool";
  entry.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  scheduled.insert_or_assign(owner.GetFormId(), std::move(entry));
}

void QuestJobRunner::OnActorLoaded(MpActor& actor) noexcept
{
  if (stopped)
    return;
  try {
    const auto jobs = QuestJournal::PersonalJobState(actor);
    if (jobs.active)
      Track(actor, *jobs.active);
  } catch (const std::exception& error) {
    spdlog::error("Quest job recovery refused owner {:x}: {}",
                  actor.GetFormId(), error.what());
  } catch (...) {
    spdlog::error("Quest job recovery refused owner {:x}", actor.GetFormId());
  }
}

bool QuestJobRunner::TimerReady(MpActor& actor, const QuestJobState::Job& job)
{
  const auto& work = job.Current();
  if (work.awaitKind != "timer" || !work.timer)
    throw std::runtime_error("Quest timer has no saved request");
  auto& owner = QuestJournal::Owner(actor);
  if (!barriers.contains(owner.GetFormId()) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  auto& barrier = barriers[owner.GetFormId()];
  if (!barrier.job) {
    barrier.job = job.id;
    barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  }
  const auto time = Now();
  if (stopped)
    throw std::runtime_error(
      "Quest coordinator was cleared while reading its clock");
  if (!barrier.timer || barrier.timer->token != work.token ||
      barrier.job != job.id) {
    // On recovery only: trust persisted UTC to credit downtime, then switch
    // permanently to this process's monotonic clock for the live countdown.
    const auto remaining = work.timer->dueUnixMs > time.unixMs
      ? std::min(work.timer->dueUnixMs - time.unixMs, work.timer->durationMs)
      : 0;
    if (remaining > std::numeric_limits<uint64_t>::max() - time.steadyMs)
      throw std::runtime_error("Quest steady deadline overflow");
    barrier.timer =
      QuestJobTimerAnchor{ work.token, time.steadyMs + remaining };
    barrier.job = job.id;
    barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  }
  return time.steadyMs >= barrier.timer->dueSteadyMs;
}

void QuestJobRunner::Retry(MpActor& actor, const std::string& command)
{
  RequireReady(actor);
  const auto jobs = QuestJournal::JobState(actor);
  if (!jobs.active || jobs.active->command != command)
    throw std::runtime_error("No active quest job to retry");
  RequireContext(actor, *jobs.active);
  Track(QuestJournal::Owner(actor), *jobs.active);
  const auto entry = scheduled.find(QuestJournal::Owner(actor).GetFormId());
  if (entry != scheduled.end()) {
    entry->second.error.clear();
    entry->second.quarantined = false;
  }
}

void QuestJobRunner::Pump() noexcept
{
  if (stopped || pumping || QuestJournal::HasActiveTransaction() ||
      scheduled.empty())
    return;
  BusyGuard guard(pumping);
  // Bound both idle lookups and execution. Round robin advances even past a
  // blocked campaign, and each campaign is considered once in this pass.
  const auto candidates = std::min<size_t>(64, scheduled.size());
  std::array<uint32_t, 64> owners{};
  auto next = scheduled.upper_bound(lastPumpedOwner);
  for (size_t index = 0; index < candidates; ++index) {
    if (next == scheduled.end())
      next = scheduled.begin();
    owners[index] = next->first;
    ++next;
  }
  size_t executed = 0;
  for (size_t index = 0; index < candidates && executed < 8 && !stopped;
       ++index) {
    auto entry = scheduled.find(owners[index]);
    if (entry == scheduled.end())
      continue;
    lastPumpedOwner = entry->first;
    if (entry->second.quarantined)
      continue;
    const auto ownerId = entry->first;
    const auto jobId = entry->second.job;
    const auto expectedOwner = entry->second.owner;
    auto quarantine = [&](const char* reason) noexcept {
      if (stopped)
        return;
      const auto current = scheduled.find(ownerId);
      if (current == scheduled.end() || current->second.job != jobId ||
          current->second.owner.lock().get() != expectedOwner.lock().get())
        return;
      current->second.quarantined = true;
      try {
        current->second.error = std::string(reason).substr(0, 1024);
        spdlog::error("Quest job {} for owner {:x} quarantined: {}", jobId,
                      ownerId, current->second.error);
      } catch (...) {
        // Keep the nonallocating quarantine bit even under allocation failure.
      }
    };
    try {
      // No references into scheduled survive a callback: clocks and host
      // hooks may clear the world, which stops and erases this coordinator.
      const auto work = entry->second;
      if (work.waitingExternal ||
          (work.contentMode == "fixture" && !schedulerFixture))
        continue;
      if (const auto barrier = barriers.find(ownerId);
          barrier != barriers.end() && barrier->second.job == jobId) {
        if (barrier->second.busy ||
            (barrier->second.receipt &&
             barrier->second.receipt->GetState() ==
               SaveReceipt::State::Pending))
          continue;
        if (barrier->second.timer) {
          const auto deadline = barrier->second.timer->dueSteadyMs;
          const auto time = Now();
          if (stopped)
            return;
          if (time.steadyMs < deadline)
            continue;
        }
      }
      const auto ownerForm = world.LookupFormByIdNoLoad(ownerId);
      const auto actorForm = world.LookupFormByIdNoLoad(work.actor);
      auto* owner = ownerForm ? ownerForm->AsActor() : nullptr;
      auto* actor = actorForm ? actorForm->AsActor() : nullptr;
      if (!owner || !actor || owner->IsDeleted() || actor->IsDeleted())
        continue;
      if (work.owner.lock().get() != owner)
        throw std::runtime_error("Scheduled quest owner was replaced");
      if (actor->GetWorldInstance() != work.instance)
        continue;
      const auto jobs = QuestJournal::PersonalJobState(*owner);
      if (!jobs.active || jobs.active->id != work.job ||
          !jobs.active->automatic) {
        scheduled.erase(ownerId);
        continue;
      }
      if (jobs.active->contentMode == "fixture" && !schedulerFixture)
        continue;
      try {
        RequireContext(*actor, *jobs.active);
      } catch (const std::exception&) {
        continue; // The initiating actor must return to the original context.
      }
      const auto observed = Inspect(*actor, work.command);
      if (observed.status != Status::Ready)
        continue;
      ++executed;
      if (jobs.active->contentMode == "fixture")
        Advance(*actor, work.command, schedulerFixture->metadata,
                schedulerFixture->scripts);
      else
        Advance(*actor, work.command);
    } catch (const std::exception& error) {
      quarantine(error.what());
    } catch (...) {
      quarantine("Unknown quest scheduler failure");
    }
  }
}

void QuestJobRunner::RequireContext(MpActor& actor,
                                    const QuestJobState::Job& job) const
{
  if (stopped || actor.GetParent() != &world || actor.IsDeleted() ||
      world.LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor)
    throw std::runtime_error("Quest job actor or coordinator is unavailable");
  const auto& owner = QuestJournal::Owner(actor);
  if (owner.IsDeleted() ||
      FormDesc::FromFormId(actor.GetFormId(), world.espmFiles).ToString() !=
        job.actor ||
      FormDesc::FromFormId(owner.GetFormId(), world.espmFiles).ToString() !=
        job.owner ||
      actor.GetWorldInstance() != job.instance)
    throw std::runtime_error("Quest job campaign context changed");
}

void QuestJobRunner::RequireReady(MpActor& actor)
{
  if (stopped || actor.GetParent() != &world)
    throw std::runtime_error("Quest job coordinator has stopped");
  const auto& owner = QuestJournal::Owner(actor);
  const auto found = barriers.find(owner.GetFormId());
  if (found == barriers.end())
    return; // On a fresh server, the loaded saved state is authoritative.
  const auto& barrier = found->second;
  if (barrier.owner.lock().get() != &owner)
    throw std::runtime_error("Quest job owner was replaced");
  if (barrier.busy ||
      (barrier.receipt &&
       barrier.receipt->GetState() == SaveReceipt::State::Pending))
    throw std::runtime_error("Quest job is waiting for its durable save");
  if (barrier.receipt &&
      barrier.receipt->GetState() != SaveReceipt::State::Durable)
    throw std::runtime_error(
      "Quest job save was abandoned; recovery is required");
}

const std::vector<PexScript::Lazy>& QuestJobRunner::Scripts()
{
  if (!manifest.empty())
    return scripts;
  auto candidate = QuestScriptRuntime::LoadScripts(world);
  // Pin one immutable set for this coordinator. Loose files are never read
  // again midway through a job. New content requires a new server lifetime.
  for (auto& lazy : candidate) {
    if (!lazy.fn)
      throw std::runtime_error("Quest content has no script loader");
    const auto loaded = lazy.fn();
    if (!loaded)
      throw std::runtime_error("Quest content has a missing script");
    lazy.fn = [loaded] { return loaded; };
  }
  const auto hash = QuestJobContent::ScriptManifest(candidate);
  scripts.swap(candidate);
  manifest = hash;
  return scripts;
}

QuestJobRunner::Result QuestJobRunner::Inspect(MpActor& actor,
                                               const std::string& command)
{
  if (stopped)
    return {
      Status::Failed, 0, false, {}, {}, "Quest coordinator was cleared"
    };
  const auto jobs = QuestJournal::JobState(actor);
  const auto* job = Find(jobs, command);
  if (!job)
    throw std::invalid_argument("Unknown quest job command");
  RequireContext(actor, *job);
  const auto& owner = QuestJournal::Owner(actor);
  if (const auto found = barriers.find(owner.GetFormId());
      found != barriers.end()) {
    const auto& barrier = found->second;
    if (barrier.owner.lock().get() != &owner)
      throw std::runtime_error("Quest job owner was replaced");
    if (barrier.job == job->id &&
        (barrier.busy ||
         (barrier.receipt &&
          barrier.receipt->GetState() == SaveReceipt::State::Pending)))
      return { Status::Pending, job->id };
    if (barrier.job == job->id && barrier.receipt &&
        barrier.receipt->GetState() == SaveReceipt::State::Abandoned)
      return {
        Status::Failed, job->id, false, {}, {}, "Quest save was abandoned"
      };
  }
  const auto& current = job->Current();
  if (const auto scheduledJob = scheduled.find(owner.GetFormId());
      scheduledJob != scheduled.end() && scheduledJob->second.job == job->id &&
      scheduledJob->second.quarantined)
    return { Status::Failed,
             job->id,
             false,
             {},
             {},
             scheduledJob->second.error.empty()
               ? "Quest scheduler quarantined this job"
               : scheduledJob->second.error };
  if (!current.failure.empty())
    return { Status::Failed, job->id, false, {}, {}, current.failure };
  if (job->cursor == "done")
    return { Status::Completed, job->id, job->accepted };
  if (current.cursor == "waiting" && current.awaitKind == "timer")
    return { TimerReady(actor, *job) ? Status::Ready : Status::Sleeping,
             job->id };
  if (current.cursor == "waiting" && !current.child)
    return { Status::Waiting, job->id, job->accepted, current.token,
             current.awaitKey };
  return { Status::Ready, job->id };
}

QuestJobRunner::Result QuestJobRunner::Submit(MpActor& actor,
                                              const std::string& command,
                                              uint32_t quest, uint16_t stage)
{
  const auto& pinned = Scripts();
  return SubmitWithContent(actor, command, quest, stage, pinned, {}, manifest,
                           false, true);
}

QuestJobRunner::Result QuestJobRunner::Submit(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  return Submit(actor, command, quest, stage,
                MetadataOverrides{ { quest, metadata } }, scripts);
}

QuestJobRunner::Result QuestJobRunner::Submit(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts, bool automatic)
{
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return SubmitWithContent(actor, command, quest, stage, scripts, overrides,
                           hashManifest, true, automatic);
}

QuestJobRunner::Result QuestJobRunner::SubmitWithContent(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait, bool automatic)
{
  if (command.empty() || command.size() > 128 ||
      command.find('\0') != std::string::npos || stopped ||
      actor.GetParent() != &world)
    throw std::invalid_argument("Invalid quest job submission");
  if (automatic && fixtureAwait && !schedulerFixture)
    throw std::runtime_error(
      "Automatic fixture jobs require scheduler content injection");
  if (automatic && fixtureAwait &&
      hashManifest !=
        QuestJobContent::WithMetadataOverrides(
          world, QuestJobContent::ScriptManifest(schedulerFixture->scripts),
          schedulerFixture->metadata))
    throw std::runtime_error(
      "Automatic fixture content differs from its scheduler provider");
  auto& owner = QuestJournal::Owner(actor);
  auto jobs = QuestJournal::JobState(actor);
  const auto questKey =
    FormDesc::FromFormId(quest, world.espmFiles).ToString();
  const auto hash = QuestJobContent::Hash(
    world, quest, Metadata(world, quest, overrides), hashManifest);
  if (const auto* prior = Find(jobs, command)) {
    RequireContext(actor, *prior);
    RequireMode(*prior, fixtureAwait);
    if (prior->quest != questKey || prior->stage != stage ||
        prior->contentHash != hash)
      throw std::runtime_error("Quest command was reused for different work");
    ValidateContent(*prior, overrides, hashManifest);
    return Inspect(actor, command);
  }
  RequireReady(actor);
  if (jobs.active || jobs.completed.size() >= QuestJobState::MaxCompleted ||
      jobs.nextJob >= 9007199254740991ULL)
    throw std::runtime_error(
      "Campaign has active work or its command history is full");
  QuestJournal::RequireAdapter(actor, quest);
  QuestJobState::Job job;
  job.id = jobs.nextJob++;
  job.generation = jobs.generation;
  job.command = command;
  job.automatic = automatic;
  job.contentMode = fixtureAwait ? "fixture" : "owned";
  job.actor =
    FormDesc::FromFormId(actor.GetFormId(), world.espmFiles).ToString();
  job.owner =
    FormDesc::FromFormId(owner.GetFormId(), world.espmFiles).ToString();
  job.quest = questKey;
  job.instance = actor.GetWorldInstance();
  job.contentHash = hash;
  job.stage = stage;
  RequireContext(actor, job);
  jobs.active = std::move(job);
  jobs.Save();
  return RunSegment(actor, std::move(jobs), sources, overrides, hashManifest,
                    {}, fixtureAwait, true);
}

void QuestJobRunner::ValidateContent(const QuestJobState::Job& job,
                                     const MetadataOverrides& overrides,
                                     const std::string& hashManifest) const
{
  const auto check = [&](const QuestJobState::Stage& work) {
    const auto quest =
      FormDesc::FromString(work.quest).ToFormId(world.espmFiles);
    if (work.contentHash !=
        QuestJobContent::Hash(world, quest, Metadata(world, quest, overrides),
                              hashManifest))
      throw std::runtime_error("Quest job content changed");
  };
  check(job);
  for (const auto& child : job.children)
    check(child);
}

QuestJobRunner::Result QuestJobRunner::Resume(MpActor& actor,
                                              const std::string& command,
                                              const std::string& token,
                                              bool completion)
{
  const auto& pinned = Scripts();
  return ResumeWithContent(actor, command, token, completion, pinned, {},
                           manifest, false);
}

QuestJobRunner::Result QuestJobRunner::Resume(
  MpActor& actor, const std::string& command, const std::string& token,
  bool completion, const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  const auto jobs = QuestJournal::JobState(actor);
  const auto* job = Find(jobs, command);
  if (!job)
    throw std::invalid_argument("Unknown quest job command");
  const auto quest =
    FormDesc::FromString(job->quest).ToFormId(world.espmFiles);
  return Resume(actor, command, token, completion,
                MetadataOverrides{ { quest, metadata } }, scripts);
}

QuestJobRunner::Result QuestJobRunner::Resume(
  MpActor& actor, const std::string& command, const std::string& token,
  bool completion, const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts)
{
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return ResumeWithContent(actor, command, token, completion, scripts,
                           overrides, hashManifest, true);
}

QuestJobRunner::Result QuestJobRunner::ResumeWithContent(
  MpActor& actor, const std::string& command, const std::string& token,
  bool completion, const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait)
{
  auto jobs = QuestJournal::JobState(actor);
  const auto* prior = Find(jobs, command);
  if (!prior)
    throw std::invalid_argument("Unknown quest job command");
  RequireContext(actor, *prior);
  RequireMode(*prior, fixtureAwait);
  ValidateContent(*prior, overrides, hashManifest);
  if (const auto found = prior->completions.find(token);
      found != prior->completions.end()) {
    if (found->second != completion)
      throw std::runtime_error("Conflicting duplicate quest completion");
    return Inspect(actor, command);
  }
  RequireReady(actor);
  if (!jobs.active || jobs.active->command != command)
    throw std::runtime_error("Quest completion does not match active work");
  const auto& current = jobs.active->Current();
  if (current.cursor != "waiting" || current.awaitKind != "bool" ||
      current.child || current.token != token || !current.failure.empty())
    throw std::runtime_error("Quest completion does not match external work");
  return RunSegment(actor, std::move(jobs), sources, overrides, hashManifest,
                    completion, fixtureAwait, false);
}

QuestJobRunner::Result QuestJobRunner::Advance(MpActor& actor,
                                               const std::string& command)
{
  const auto& pinned = Scripts();
  return AdvanceWithContent(actor, command, pinned, {}, manifest, false);
}

QuestJobRunner::Result QuestJobRunner::Advance(
  MpActor& actor, const std::string& command,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts)
{
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return AdvanceWithContent(actor, command, scripts, overrides, hashManifest,
                            true);
}

QuestJobRunner::Result QuestJobRunner::AdvanceWithContent(
  MpActor& actor, const std::string& command,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait)
{
  auto jobs = QuestJournal::JobState(actor);
  const auto* prior = Find(jobs, command);
  if (!prior)
    throw std::invalid_argument("Unknown quest job command");
  RequireContext(actor, *prior);
  RequireMode(*prior, fixtureAwait);
  ValidateContent(*prior, overrides, hashManifest);
  const auto observed = Inspect(actor, command);
  if (observed.status != Status::Ready)
    return observed;
  RequireReady(actor);
  const auto& current = jobs.active->Current();
  const bool consumeChild = current.node && current.cursor == "done";
  const bool timer =
    current.cursor == "waiting" && current.awaitKind == "timer";
  if (current.cursor != "begin" && !consumeChild && !timer)
    throw std::runtime_error("Quest job has no internal work ready");
  return RunSegment(actor, std::move(jobs), sources, overrides, hashManifest,
                    timer ? std::optional(QuestJobState::Completion::None())
                          : std::nullopt,
                    fixtureAwait, false, consumeChild);
}

QuestJobRunner::Result QuestJobRunner::RunSegment(
  MpActor& actor, QuestJobState jobs,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  std::optional<QuestJobState::Completion> completion, bool fixtureAwait,
  bool submit, bool consumeChild)
{
  auto& owner = QuestJournal::Owner(actor);
  auto& job = *jobs.active;
  const auto ownerId = owner.GetFormId();
  const auto command = job.command;
  Track(owner, job);
  // Preallocate the barrier before committing effects. Publishing its exact
  // receipt must not allocate or throw after AtomicJob has committed.
  if (!barriers.contains(owner.GetFormId()) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  auto& barrier = barriers[owner.GetFormId()];
  barrier.job = job.id;
  barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  {
    BusyGuard guard(barrier.busy);
    std::optional<QuestJobTimerAnchor> timerAnchor;
    const auto receipt = QuestJournal::AtomicJob(actor, job.id, [&] {
      const auto requireAdapter = [&](const QuestJobState::Stage& stage) {
        QuestJournal::RequireAdapter(
          actor, FormDesc::FromString(stage.quest).ToFormId(world.espmFiles));
      };
      requireAdapter(job);
      for (const auto& child : job.children)
        requireAdapter(child);
      if (submit)
        QuestJournal::SetJobState(actor, jobs);
      if (consumeChild) {
        const auto child = job.children.back();
        job.children.pop_back();
        auto& parent = job.Current();
        if (parent.cursor != "waiting" || parent.child != child.node ||
            parent.awaitKey != "quest-stage")
          throw std::runtime_error("Quest child result lost its parent");
        parent.child = 0;
        completion = child.accepted;
      }
      const auto quest =
        FormDesc::FromString(job.Current().quest).ToFormId(world.espmFiles);
      QuestJournal::RequireAdapter(actor, quest);
      QuestScriptRuntime::ExecuteJobSegment(
        actor, job, Metadata(world, quest, overrides), sources, completion,
        fixtureAwait, overrides, hashManifest, [this] { return Now(); },
        &timerAnchor);
      ++job.segment;
      if (job.cursor == "done") {
        jobs.completed.push_back(job);
        jobs.active.reset();
      }
      QuestJournal::SetJobState(actor, jobs);
    });
    barrier.timer = std::move(timerAnchor);
    barrier.receipt = receipt;
    const auto scheduledJob = scheduled.find(ownerId);
    if (scheduledJob != scheduled.end() &&
        scheduledJob->second.command == command)
      scheduledJob->second.waitingExternal = jobs.active &&
        jobs.active->Current().cursor == "waiting" &&
        jobs.active->Current().awaitKind == "bool";
  }
  return Inspect(actor, command);
}
