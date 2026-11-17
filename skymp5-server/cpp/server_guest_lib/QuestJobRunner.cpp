#include "QuestJobRunner.h"
#include "MpActor.h"
#include "QuestJobContent.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"
#include "WorldState.h"
#include <algorithm>
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
{
}

void QuestJobRunner::Stop() noexcept
{
  // Reusing a cleared world is not recovery: an abandoned write may still
  // commit after objects were removed. Reconstruct a server from storage to
  // recover jobs, after its previous writer has shut down.
  stopped = true;
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
  if (!current.failure.empty())
    return { Status::Failed, job->id, false, {}, {}, current.failure };
  if (job->cursor == "done")
    return { Status::Completed, job->id, job->accepted };
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
                           false);
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
  const std::vector<PexScript::Lazy>& scripts)
{
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return SubmitWithContent(actor, command, quest, stage, scripts, overrides,
                           hashManifest, true);
}

QuestJobRunner::Result QuestJobRunner::SubmitWithContent(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait)
{
  if (command.empty() || command.size() > 128 ||
      command.find('\0') != std::string::npos || stopped ||
      actor.GetParent() != &world)
    throw std::invalid_argument("Invalid quest job submission");
  auto& owner = QuestJournal::Owner(actor);
  auto jobs = QuestJournal::JobState(actor);
  const auto questKey =
    FormDesc::FromFormId(quest, world.espmFiles).ToString();
  const auto hash = QuestJobContent::Hash(
    world, quest, Metadata(world, quest, overrides), hashManifest);
  if (const auto* prior = Find(jobs, command)) {
    RequireContext(actor, *prior);
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
  if (current.cursor != "waiting" || current.child || current.token != token ||
      !current.failure.empty())
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
  ValidateContent(*prior, overrides, hashManifest);
  const auto observed = Inspect(actor, command);
  if (observed.status != Status::Ready)
    return observed;
  RequireReady(actor);
  const auto& current = jobs.active->Current();
  const bool consumeChild = current.node && current.cursor == "done";
  if (current.cursor != "begin" && !consumeChild)
    throw std::runtime_error("Quest job has no internal work ready");
  return RunSegment(actor, std::move(jobs), sources, overrides, hashManifest,
                    {}, fixtureAwait, false, consumeChild);
}

QuestJobRunner::Result QuestJobRunner::RunSegment(
  MpActor& actor, QuestJobState jobs,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  std::optional<bool> completion, bool fixtureAwait, bool submit,
  bool consumeChild)
{
  auto& owner = QuestJournal::Owner(actor);
  auto& job = *jobs.active;
  const auto command = job.command;
  // Preallocate the barrier before committing effects. Publishing its exact
  // receipt must not allocate or throw after AtomicJob has committed.
  if (!barriers.contains(owner.GetFormId()) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  auto& barrier = barriers[owner.GetFormId()];
  barrier.job = job.id;
  barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  {
    BusyGuard guard(barrier.busy);
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
        fixtureAwait, overrides, hashManifest);
      ++job.segment;
      if (job.cursor == "done") {
        jobs.completed.push_back(job);
        jobs.active.reset();
      }
      QuestJournal::SetJobState(actor, jobs);
    });
    barrier.receipt = receipt;
  }
  return Inspect(actor, command);
}
