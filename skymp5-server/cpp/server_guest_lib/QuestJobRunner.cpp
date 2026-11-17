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

espm::QuestScriptData Metadata(WorldState& world, uint32_t id)
{
  const auto record = world.GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "QUST")
    throw std::invalid_argument("Expected a live quest record");
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
  if (!job->failure.empty())
    return { Status::Failed, job->id, false, {}, {}, job->failure };
  if (job->cursor == "done")
    return { Status::Completed, job->id, job->accepted };
  if (job->cursor == "waiting")
    return { Status::Waiting, job->id, job->accepted, job->token,
             job->awaitKey };
  return { Status::Pending, job->id };
}

QuestJobRunner::Result QuestJobRunner::Submit(MpActor& actor,
                                              const std::string& command,
                                              uint32_t quest, uint16_t stage)
{
  const auto metadata = Metadata(world, quest);
  const auto& pinned = Scripts();
  return SubmitWithContent(
    actor, command, quest, stage, metadata, pinned,
    QuestJobContent::Hash(world, quest, metadata, manifest), false);
}

QuestJobRunner::Result QuestJobRunner::Submit(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  return SubmitWithContent(
    actor, command, quest, stage, metadata, scripts,
    QuestJobContent::Hash(world, quest, metadata, scripts), true);
}

QuestJobRunner::Result QuestJobRunner::SubmitWithContent(
  MpActor& actor, const std::string& command, uint32_t quest, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& sources, const std::string& hash,
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
  if (const auto* prior = Find(jobs, command)) {
    RequireContext(actor, *prior);
    if (prior->quest != questKey || prior->stage != stage ||
        prior->contentHash != hash)
      throw std::runtime_error("Quest command was reused for different work");
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
  jobs.active = job;
  jobs.Save(); // All request/schema checks precede script execution.
  if (!barriers.contains(owner.GetFormId()) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  auto& barrier = barriers[owner.GetFormId()];
  barrier.job = job.id;
  barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  {
    BusyGuard guard(barrier.busy);
    const auto receipt = QuestJournal::AtomicJob(actor, job.id, [&] {
      QuestJournal::SetJobState(actor, jobs);
      QuestScriptRuntime::ExecuteJobSegment(actor, *jobs.active, metadata,
                                            sources, {}, fixtureAwait);
      ++jobs.active->segment;
      if (jobs.active->cursor == "done") {
        jobs.completed.push_back(*jobs.active);
        jobs.active.reset();
      }
      QuestJournal::SetJobState(actor, jobs);
    });
    barrier.receipt = receipt;
  }
  return Inspect(actor, command);
}

QuestJobRunner::Result QuestJobRunner::Resume(MpActor& actor,
                                              const std::string& command,
                                              const std::string& token,
                                              bool completion)
{
  const auto jobs = QuestJournal::JobState(actor);
  const auto* job = Find(jobs, command);
  if (!job)
    throw std::invalid_argument("Unknown quest job command");
  const auto quest =
    FormDesc::FromString(job->quest).ToFormId(world.espmFiles);
  const auto metadata = Metadata(world, quest);
  const auto& pinned = Scripts();
  return ResumeWithContent(
    actor, command, token, completion, metadata, pinned,
    QuestJobContent::Hash(world, quest, metadata, manifest), false);
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
  return ResumeWithContent(
    actor, command, token, completion, metadata, scripts,
    QuestJobContent::Hash(world, quest, metadata, scripts), true);
}

QuestJobRunner::Result QuestJobRunner::ResumeWithContent(
  MpActor& actor, const std::string& command, const std::string& token,
  bool completion, const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& sources, const std::string& hash,
  bool fixtureAwait)
{
  auto jobs = QuestJournal::JobState(actor);
  const auto* prior = Find(jobs, command);
  if (!prior)
    throw std::invalid_argument("Unknown quest job command");
  RequireContext(actor, *prior);
  if (prior->contentHash != hash)
    throw std::runtime_error("Quest job content changed");
  if (const auto found = prior->completions.find(token);
      found != prior->completions.end()) {
    if (found->second != completion)
      throw std::runtime_error("Conflicting duplicate quest completion");
    return Inspect(actor, command);
  }
  RequireReady(actor);
  if (!jobs.active || jobs.active->command != command ||
      jobs.active->cursor != "waiting" || jobs.active->token != token ||
      !jobs.active->failure.empty())
    throw std::runtime_error("Quest completion does not match active work");
  auto& owner = QuestJournal::Owner(actor);
  QuestJournal::RequireAdapter(
    actor, FormDesc::FromString(jobs.active->quest).ToFormId(world.espmFiles));
  if (!barriers.contains(owner.GetFormId()) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  auto& barrier = barriers[owner.GetFormId()];
  barrier.job = jobs.active->id;
  barrier.owner = world.LookupFormByIdNoLoad(owner.GetFormId());
  {
    BusyGuard guard(barrier.busy);
    const auto receipt = QuestJournal::AtomicJob(actor, jobs.active->id, [&] {
      QuestScriptRuntime::ExecuteJobSegment(actor, *jobs.active, metadata,
                                            sources, completion, fixtureAwait);
      ++jobs.active->segment;
      if (jobs.active->cursor == "done") {
        jobs.completed.push_back(*jobs.active);
        jobs.active.reset();
      }
      QuestJournal::SetJobState(actor, jobs);
    });
    barrier.receipt = receipt;
  }
  return Inspect(actor, command);
}
