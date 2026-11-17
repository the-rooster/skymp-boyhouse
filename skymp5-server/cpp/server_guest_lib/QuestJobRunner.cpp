#include "QuestJobRunner.h"
#include "CampaignMovement.h"
#include "CampaignReferenceBinding.h"
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
constexpr uint64_t MaxIdentity = 9007199254740991ULL;

bool Matches(const QuestJobState::Job& job,
             const QuestJobRunner::Request& request)
{
  return request.sequence
    ? job.sequenced && job.id == request.sequence->sequence &&
      job.generation == request.sequence->generation &&
      job.owner == request.sequence->owner
    : !job.sequenced && job.command == request.command;
}

QuestJobRunner::Request RequestFor(const QuestJobState::Job& job)
{
  if (job.sequenced)
    return QuestJobRunner::RequestId{ job.owner, job.generation, job.id };
  return job.command;
}

const QuestJobState::Job* Find(const QuestJobState& state,
                               const QuestJobRunner::Request& command)
{
  for (const auto& [id, job] : state.live)
    if (Matches(job, command))
      return &job;
  for (const auto& job : state.completed)
    if (Matches(job, command))
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

QuestJobRunner::Request::Request(std::string value)
  : command(std::move(value))
{
}

QuestJobRunner::Request::Request(const char* value)
  : command(value ? value : "")
{
  if (!value)
    throw std::invalid_argument("Null quest command");
}

QuestJobRunner::Request::Request(RequestId id)
  : command("quest-request:" + std::to_string(id.generation) + ":" +
            std::to_string(id.sequence))
  , sequence(std::move(id))
{
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

QuestJobRunner::RequestId QuestJobRunner::NextRequest(MpActor& actor)
{
  const auto jobs = QuestJournal::JobState(actor);
  const auto& owner = QuestJournal::Owner(actor);
  RequestId id{ owner.GetChangeForm().formDesc.ToString(), jobs.generation,
                jobs.nextJob };
  RequireRequest(actor, id, jobs);
  if (id.sequence >= MaxIdentity)
    throw std::overflow_error("Quest request identities exhausted");
  return id;
}

QuestJobRunner::OccurrenceId QuestJobRunner::NextOccurrence(MpActor& actor)
{
  const auto jobs = QuestJournal::JobState(actor);
  auto& owner = QuestJournal::Owner(actor);
  OccurrenceId id{ owner.GetChangeForm().formDesc.ToString(),
                   CampaignReferenceBinding::GetIdentity(owner),
                   jobs.generation, jobs.ingress.next };
  RequireOccurrence(actor, id, jobs);
  if (id.sequence >= MaxIdentity)
    throw std::overflow_error("Quest occurrence identities exhausted");
  return id;
}

void QuestJobRunner::RequireOccurrence(MpActor& actor, const OccurrenceId& id,
                                       const QuestJobState& jobs) const
{
  // An occurrence sequence is not a global job ID. Validate the campaign
  // using its next job, which cannot accidentally name a legacy command.
  RequireRequest(actor, RequestId{ id.owner, id.generation, jobs.nextJob },
                 jobs);
  auto& owner = QuestJournal::Owner(actor);
  const auto identity = CampaignReferenceBinding::GetIdentity(owner);
  if (!id.sequence || id.sequence > MaxIdentity ||
      id.campaignIdentity != identity ||
      (!jobs.ingress.identity.empty() && jobs.ingress.identity != identity))
    throw std::runtime_error(
      "Quest occurrence belongs to another campaign identity");
}

QuestJobRunner::CapturedOccurrence QuestJobRunner::CaptureOccurrence(
  MpActor& actor, const OccurrenceId& id, MpObjectReference& target,
  const std::string& name, MpObjectReference& source)
{
  const auto jobs = QuestJournal::JobState(actor);
  RequireOccurrence(actor, id, jobs);
  RequireReady(actor);
  if (world.IsRestoring())
    throw std::runtime_error(
      "Cannot capture quest occurrence during restoration");
  if (name != "OnTriggerEnter" && name != "OnTriggerLeave" &&
      name != "OnTrigger")
    throw std::invalid_argument("Unsupported native quest occurrence");
  return {
    id, actor.GetChangeForm().formDesc.ToString(), actor.GetWorldInstance(),
    QuestScriptRuntime::CaptureInteraction(actor, target, name, source)
  };
}

QuestJobRunner::AdmissionResult QuestJobRunner::InspectOccurrence(
  MpActor& actor, const OccurrenceId& id)
{
  return InspectOccurrence(actor, id, QuestJournal::JobState(actor));
}

QuestJobRunner::AdmissionResult QuestJobRunner::InspectOccurrence(
  MpActor& actor, const OccurrenceId& id, const QuestJobState& jobs)
{
  RequireOccurrence(actor, id, jobs);
  const auto* job = jobs.FindOccurrence(id.sequence);
  std::optional<RequestId> request;
  if (job)
    request = RequestId{ job->owner, job->generation, job->id };
  const auto& owner = QuestJournal::Owner(actor);
  if (const auto found = barriers.find(owner.GetFormId());
      found != barriers.end()) {
    const auto& barrier = found->second;
    if (barrier.owner.lock().get() != &owner)
      throw std::runtime_error("Quest occurrence owner was replaced");
    // A callback must not acknowledge a transaction-local occurrence before
    // its exact admission receipt has been published.
    if (barrier.busy && barrier.admittingOccurrence <= id.sequence &&
        id.sequence <= barrier.admittingLastOccurrence &&
        barrier.admittingGeneration == id.generation)
      return { AdmissionStatus::Pending, std::move(request) };
    if (barrier.occurrence <= id.sequence &&
        id.sequence <= barrier.lastOccurrence &&
        barrier.occurrenceGeneration == id.generation &&
        barrier.admissionReceipt) {
      const auto state = barrier.admissionReceipt->GetState();
      if (state == SaveReceipt::State::Pending)
        return { AdmissionStatus::Pending, std::move(request) };
      if (state != SaveReceipt::State::Durable)
        return {
          AdmissionStatus::Failed, std::move(request),
          "Quest occurrence admission was abandoned; recovery is required"
        };
    }
  }
  if (!job && !jobs.IsOccurrenceRetired(id.sequence))
    throw std::invalid_argument("Unknown quest occurrence");
  // Subsequent handler or retirement receipts cannot revoke an admission.
  // After recovery, the loaded occurrence ledger is authoritative.
  return { AdmissionStatus::Durable, std::move(request) };
}

std::optional<QuestJobRunner::AdmissionBatchResult>
QuestJobRunner::PrepareOccurrences(
  MpActor& actor, const std::vector<CapturedOccurrence>& occurrences)
{
  auto jobs = QuestJournal::JobState(actor);
  auto& owner = QuestJournal::Owner(actor);
  RequireRequest(actor,
                 RequestId{ owner.GetChangeForm().formDesc.ToString(),
                            jobs.generation, jobs.nextJob },
                 jobs);
  if (occurrences.size() > QuestJobState::MaxBatchOccurrences)
    throw std::runtime_error("Native occurrence batch limit exceeded");
  size_t existingCount = 0;
  for (size_t index = 0; index < occurrences.size(); ++index) {
    const auto& occurrence = occurrences[index];
    RequireOccurrence(actor, occurrence.id, jobs);
    if (index &&
        occurrence.id.sequence != occurrences.front().id.sequence + index)
      throw std::runtime_error("Native occurrence batch is not contiguous");
    if (const auto* prior = jobs.FindOccurrence(occurrence.id.sequence)) {
      if (prior->actor != actor.GetChangeForm().formDesc.ToString() ||
          prior->actor != occurrence.actor ||
          prior->instance != occurrence.instance ||
          prior->interaction != occurrence.interaction)
        throw std::runtime_error(
          "Quest occurrence was reused for different work");
      ++existingCount;
    } else if (jobs.IsOccurrenceRetired(occurrence.id.sequence))
      ++existingCount;
  }
  const auto found = barriers.find(owner.GetFormId());
  if (found != barriers.end() && found->second.owner.lock().get() != &owner)
    throw std::runtime_error("Quest occurrence owner was replaced");
  if (!occurrences.empty() && existingCount == occurrences.size()) {
    AdmissionBatchResult result;
    result.status = AdmissionStatus::Durable;
    result.results.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
      auto item = InspectOccurrence(actor, occurrence.id, jobs);
      if (item.status == AdmissionStatus::Failed ||
          (item.status == AdmissionStatus::Pending &&
           result.status != AdmissionStatus::Failed))
        result.status = item.status;
      result.results.push_back(std::move(item));
    }
    if (found != barriers.end() &&
        found->second.occurrenceGeneration == jobs.generation &&
        occurrences.front().id.sequence >= found->second.occurrence &&
        occurrences.back().id.sequence <= found->second.lastOccurrence)
      result.receipt = found->second.admissionReceipt;
    return result;
  }
  // A reentrant callback cannot expose partially installed roots as a newly
  // admitted batch. Older already-durable batches remain queryable above.
  if (found != barriers.end() && found->second.busy) {
    AdmissionBatchResult pending;
    pending.results.resize(occurrences.size());
    for (size_t index = 0; index < occurrences.size(); ++index)
      if (const auto* job =
            jobs.FindOccurrence(occurrences[index].id.sequence))
        pending.results[index].request =
          RequestId{ job->owner, job->generation, job->id };
    return pending;
  }
  if (existingCount)
    throw std::runtime_error("Cannot mix existing and new native occurrences");
  RequireReady(actor);
  if (world.IsRestoring())
    throw std::runtime_error(
      "Cannot admit quest occurrence during restoration");
  world.RequireDurableSaveStorage();
  if (!occurrences.empty() &&
      occurrences.front().id.sequence != jobs.ingress.next)
    throw std::runtime_error(
      "Quest occurrence is not the next admission identity");
  if (occurrences.size() > QuestJobState::MaxLive - jobs.live.size() ||
      occurrences.size() > MaxIdentity - jobs.nextJob ||
      occurrences.size() > MaxIdentity - jobs.ingress.next)
    throw std::runtime_error("Campaign has no native occurrence capacity");
  for (const auto& occurrence : occurrences)
    if (occurrence.actor != actor.GetChangeForm().formDesc.ToString() ||
        occurrence.instance != actor.GetWorldInstance() ||
        (occurrence.interaction.name != "OnTriggerEnter" &&
         occurrence.interaction.name != "OnTriggerLeave" &&
         occurrence.interaction.name != "OnTrigger"))
      throw std::runtime_error("Quest occurrence capture context changed");
  if (!occurrences.empty()) {
    for (const auto& [id, live] : jobs.live)
      if (live.environmentHash.empty())
        throw std::runtime_error(
          "Concurrent admission requires a pinned environment");
    // Preflight retention before loading content, without consuming
    // identities.
    jobs.RetireForAdmission();
  }
  return std::nullopt;
}

QuestJobRunner::AdmissionResult QuestJobRunner::AdmitOccurrence(
  MpActor& actor, const CapturedOccurrence& occurrence)
{
  return AdmitOccurrences(actor, { occurrence }).results.front();
}

QuestJobRunner::AdmissionResult QuestJobRunner::AdmitOccurrence(
  MpActor& actor, const CapturedOccurrence& occurrence,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources, bool automatic)
{
  return AdmitOccurrences(actor, { occurrence }, overrides, sources, automatic)
    .results.front();
}

QuestJobRunner::AdmissionBatchResult QuestJobRunner::AdmitOccurrences(
  MpActor& actor, const std::vector<CapturedOccurrence>& occurrences)
{
  if (const auto existing = PrepareOccurrences(actor, occurrences))
    return *existing;
  if (occurrences.empty())
    return AdmitOccurrencesWithContent(actor, occurrences, {}, {}, {}, false,
                                       true);
  const auto captured = occurrences;
  const auto& pinned = Scripts();
  return AdmitOccurrencesWithContent(actor, captured, pinned, {}, manifest,
                                     false, true);
}

QuestJobRunner::AdmissionBatchResult QuestJobRunner::AdmitOccurrences(
  MpActor& actor, const std::vector<CapturedOccurrence>& occurrences,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources, bool automatic)
{
  if (const auto existing = PrepareOccurrences(actor, occurrences))
    return *existing;
  if (occurrences.empty())
    return AdmitOccurrencesWithContent(actor, occurrences, {}, {}, {}, true,
                                       automatic);
  const auto captured = occurrences;
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(sources), overrides);
  return AdmitOccurrencesWithContent(actor, captured, sources, overrides,
                                     hashManifest, true, automatic);
}

QuestJobRunner::AdmissionBatchResult QuestJobRunner::AdmitMovement(
  MpActor& requester, const std::shared_ptr<CampaignMovementPlan>& plan)
{
  const auto movement = plan;
  const auto captured = CaptureMovement(requester, movement);
  if (captured.empty())
    return AdmitOccurrencesWithContent(requester, captured, {}, {}, {}, false,
                                       true, movement);
  const auto& pinned = Scripts();
  return AdmitOccurrencesWithContent(requester, captured, pinned, {}, manifest,
                                     false, true, movement);
}

QuestJobRunner::AdmissionBatchResult QuestJobRunner::AdmitMovement(
  MpActor& requester, const std::shared_ptr<CampaignMovementPlan>& plan,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources, bool automatic)
{
  const auto movement = plan;
  const auto captured = CaptureMovement(requester, movement);
  if (captured.empty())
    return AdmitOccurrencesWithContent(requester, captured, {}, {}, {}, true,
                                       automatic, movement);
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(sources), overrides);
  return AdmitOccurrencesWithContent(requester, captured, sources, overrides,
                                     hashManifest, true, automatic, movement);
}

std::vector<QuestJobRunner::CapturedOccurrence>
QuestJobRunner::CaptureMovement(
  MpActor& requester, const std::shared_ptr<CampaignMovementPlan>& movement)
{
  if (!movement)
    throw std::invalid_argument("Missing campaign movement plan");
  movement->Validate(requester);
  RequireReady(requester);
  const auto& events = movement->GetEvents();
  if (events.size() > QuestJobState::MaxBatchOccurrences)
    throw std::runtime_error("Movement occurrence batch limit exceeded");
  std::vector<CapturedOccurrence> captured;
  if (!events.empty()) {
    auto id = NextOccurrence(requester);
    const auto jobs = QuestJournal::JobState(requester);
    if (events.size() > QuestJobState::MaxLive - jobs.live.size() ||
        events.size() > MaxIdentity - jobs.nextJob ||
        events.size() > MaxIdentity - id.sequence)
      throw std::runtime_error("Campaign has no movement occurrence capacity");
    captured.reserve(events.size());
    for (const auto& event : events) {
      auto* target =
        event.target ? event.target->AsObjectReference() : nullptr;
      if (!target)
        throw std::runtime_error("Movement occurrence lost its target");
      captured.push_back(CaptureOccurrence(requester, id, *target, event.name,
                                           movement->GetSource()));
      ++id.sequence;
    }
  }
  movement->Validate(requester);
  if (PrepareOccurrences(requester, captured))
    throw std::runtime_error("Movement requires a fresh occurrence admission");
  return captured;
}

QuestJobRunner::AdmissionBatchResult
QuestJobRunner::AdmitOccurrencesWithContent(
  MpActor& actor, const std::vector<CapturedOccurrence>& occurrences,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait, bool automatic,
  std::shared_ptr<CampaignMovementPlan> movement)
{
  if (automatic && fixtureAwait && !occurrences.empty()) {
    if (!schedulerFixture ||
        hashManifest !=
          QuestJobContent::WithMetadataOverrides(
            world, QuestJobContent::ScriptManifest(schedulerFixture->scripts),
            schedulerFixture->metadata))
      throw std::runtime_error(
        "Automatic fixture content differs from its scheduler provider");
  }
  const auto environment = occurrences.empty()
    ? std::string{}
    : QuestJobContent::Environment(world, hashManifest);
  // Script loaders may invoke host callbacks. Revalidate after every loader
  // and content calculation, before preallocating scheduler/admission state.
  if (movement) {
    movement->Validate(actor);
    RequireReady(actor);
    const auto current = QuestJournal::JobState(actor);
    if (!occurrences.empty() &&
        occurrences.front().id.sequence != current.ingress.next)
      throw std::runtime_error("Movement occurrence identity was consumed");
  }
  if (const auto existing = PrepareOccurrences(actor, occurrences)) {
    if (movement)
      throw std::runtime_error("Movement cannot reuse an occurrence receipt");
    return *existing;
  }
  auto& owner = QuestJournal::Owner(actor);
  auto jobs = QuestJournal::JobState(actor);
  if (!occurrences.empty())
    for (const auto& [id, live] : jobs.live)
      if (live.environmentHash != environment ||
          live.contentMode != (fixtureAwait ? "fixture" : "owned"))
        throw std::runtime_error(
          "Concurrent quest jobs require one pinned environment");
  std::vector<uint64_t> ids, retiredIds;
  std::vector<QuestJobState::Job> roots;
  AdmissionBatchResult result;
  ids.reserve(occurrences.size());
  roots.reserve(occurrences.size());
  result.results.reserve(occurrences.size());
  for (size_t index = 0; index < occurrences.size(); ++index) {
    const auto& occurrence = occurrences[index];
    QuestJobState::Job job;
    job.kind = QuestJobState::Kind::Interaction;
    job.cursor = "done";
    job.interactionDone = false;
    job.interaction = occurrence.interaction;
    job.occurrence = occurrence.id.sequence;
    job.id = jobs.nextJob + index;
    job.generation = jobs.generation;
    job.actor = occurrence.actor;
    job.owner = occurrence.id.owner;
    job.instance = occurrence.instance;
    job.command =
      Request(RequestId{ job.owner, job.generation, job.id }).command;
    job.sequenced = true;
    job.automatic = automatic;
    job.contentMode = fixtureAwait ? "fixture" : "owned";
    job.environmentHash = environment;
    RequireContext(actor, job);
    ids.push_back(job.id);
    result.results.push_back(
      { AdmissionStatus::Pending,
        RequestId{ job.owner, job.generation, job.id } });
    roots.push_back(std::move(job));
  }
  const auto ownerId = owner.GetFormId();
  if (!barriers.contains(ownerId) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  Barrier initialBarrier;
  initialBarrier.owner = world.LookupFormByIdNoLoad(ownerId);
  auto& barrier =
    barriers.try_emplace(ownerId, std::move(initialBarrier)).first->second;
  try {
    for (const auto& root : roots)
      Track(owner, root);
    barrier.job = ids.empty() ? 0 : ids.front();
    barrier.lastJob = ids.empty() ? 0 : ids.back();
    barrier.generation = jobs.generation;
    BusyGuard guard(barrier.busy);
    struct AdmissionGuard
    {
      Barrier& barrier;
      ~AdmissionGuard()
      {
        barrier.admittingOccurrence = barrier.admittingLastOccurrence =
          barrier.admittingGeneration = 0;
      }
    } admissionGuard{ barrier };
    if (!occurrences.empty()) {
      barrier.admittingOccurrence = occurrences.front().id.sequence;
      barrier.admittingLastOccurrence = occurrences.back().id.sequence;
      barrier.admittingGeneration = jobs.generation;
    }
    auto committed = QuestJournal::AtomicJobAdmissionRoots(
      actor, roots,
      [&] {
        if (movement)
          QuestJournal::StageMovement(actor, movement);
      },
      [&](QuestJobState::Job& job) {
        QuestScriptRuntime::ExecuteJobSegment(
          actor, job, {}, sources, {}, fixtureAwait, overrides, hashManifest,
          [this] { return Now(); });
        ++job.segment;
      });
    result.receipt = std::move(committed.receipt);
    retiredIds = std::move(committed.retiredIds);
    // Everything after commit is nonallocating: all results and scheduler
    // slots were prepared before entering the transaction.
    barrier.receipt = result.receipt;
    if (!occurrences.empty()) {
      barrier.occurrence = occurrences.front().id.sequence;
      barrier.lastOccurrence = occurrences.back().id.sequence;
      barrier.occurrenceGeneration = jobs.generation;
      barrier.admissionReceipt = result.receipt;
    }
    if (!retiredIds.empty()) {
      barrier.retiredIds = std::move(retiredIds);
      barrier.retirementGeneration = jobs.generation;
      barrier.retirementReceipt = result.receipt;
    }
    result.status = result.receipt->GetState() == SaveReceipt::State::Durable
      ? AdmissionStatus::Durable
      : AdmissionStatus::Pending;
    for (auto& item : result.results)
      item.status = result.status;
  } catch (...) {
    if (const auto campaign = scheduled.find(ownerId);
        campaign != scheduled.end())
      for (const auto id : ids)
        campaign->second.jobs.erase(id);
    throw;
  }
  return result;
}

void QuestJobRunner::RequireRequest(MpActor& actor, const Request& request,
                                    const QuestJobState& jobs) const
{
  if (stopped || actor.GetParent() != &world || actor.IsDeleted() ||
      world.LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor)
    throw std::runtime_error(
      "Quest request actor or coordinator is unavailable");
  const auto& owner = QuestJournal::Owner(actor);
  if (owner.IsDeleted() ||
      world.LookupFormByIdNoLoad(owner.GetFormId()).get() != &owner)
    throw std::runtime_error("Quest request campaign owner is unavailable");
  if (const auto& id = request.sequence) {
    if (id->owner != owner.GetChangeForm().formDesc.ToString() ||
        id->generation != jobs.generation || !id->sequence ||
        id->sequence > MaxIdentity)
      throw std::runtime_error(
        "Quest request belongs to another campaign or generation");
    const auto legacyId = [&](const QuestJobState::Job& job) {
      return !job.sequenced && job.id == id->sequence;
    };
    if (std::any_of(
          jobs.live.begin(), jobs.live.end(),
          [&](const auto& entry) { return legacyId(entry.second); }) ||
        std::any_of(jobs.completed.begin(), jobs.completed.end(), legacyId))
      throw std::runtime_error("Numbered request identifies a legacy command");
  }
}

std::optional<QuestJobRunner::Result> QuestJobRunner::Retired(
  MpActor& actor, const Request& request, const QuestJobState& jobs)
{
  RequireRequest(actor, request, jobs);
  if (!request.sequence || !jobs.IsRetired(request.sequence->sequence))
    return std::nullopt;
  const auto id = request.sequence->sequence;
  const auto& owner = QuestJournal::Owner(actor);
  if (const auto found = barriers.find(owner.GetFormId());
      found != barriers.end()) {
    const auto& barrier = found->second;
    if (barrier.owner.lock().get() != &owner)
      throw std::runtime_error("Quest retirement owner was replaced");
    // A reentrant native callback must not observe a transaction-local
    // retirement before its receipt can be published.
    if (barrier.busy)
      return Result{ Status::Pending, id };
    if (barrier.retirementGeneration == jobs.generation &&
        std::find(barrier.retiredIds.begin(), barrier.retiredIds.end(), id) !=
          barrier.retiredIds.end() &&
        barrier.retirementReceipt) {
      const auto state = barrier.retirementReceipt->GetState();
      if (state == SaveReceipt::State::Pending)
        return Result{ Status::Pending, id };
      if (state != SaveReceipt::State::Durable)
        return Result{
          Status::Failed,
          id,
          false,
          {},
          {},
          "Quest retirement was abandoned; recovery is required"
        };
    }
  }
  return Result{ Status::Retired, id };
}

std::optional<QuestJobRunner::Result> QuestJobRunner::Retired(
  MpActor& actor, const Request& request)
{
  if (!request.sequence)
    return std::nullopt;
  return Retired(actor, request, QuestJournal::JobState(actor));
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
      std::any_of(barriers.begin(), barriers.end(),
                  [](const auto& entry) { return entry.second.busy; }) ||
      std::any_of(
        scheduled.begin(), scheduled.end(), [](const auto& campaign) {
          return std::any_of(
            campaign.second.jobs.begin(), campaign.second.jobs.end(),
            [](const auto& entry) { return entry.second.timer.has_value(); });
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
  if (stopped)
    return;
  if (job.owner != owner.GetChangeForm().formDesc.ToString())
    throw std::runtime_error(
      "Quest scheduling owner does not match saved work");
  const auto ownerId = owner.GetFormId();
  auto campaign = scheduled.find(ownerId);
  if (campaign == scheduled.end()) {
    if (scheduled.size() >= 4096)
      throw std::runtime_error("Quest scheduler campaign limit exceeded");
    CampaignWork work;
    work.owner = world.LookupFormByIdNoLoad(ownerId);
    campaign = scheduled.emplace(ownerId, std::move(work)).first;
  }
  if (campaign->second.owner.lock().get() != &owner)
    throw std::runtime_error("Quest scheduling owner was replaced");
  auto& entries = campaign->second.jobs;
  const auto found = entries.find(job.id);
  if (found != entries.end() && found->second.generation == job.generation &&
      found->second.sequenced == job.sequenced &&
      found->second.command == job.command &&
      found->second.instance == job.instance &&
      found->second.contentMode == job.contentMode &&
      found->second.automatic == job.automatic &&
      found->second.actor ==
        FormDesc::FromString(job.actor).ToFormId(world.espmFiles))
    return;
  if (found == entries.end() && entries.size() >= QuestJobState::MaxLive)
    throw std::runtime_error("Quest scheduler live-job limit exceeded");
  Scheduled entry;
  entry.job = job.id;
  entry.generation = job.generation;
  entry.sequenced = job.sequenced;
  entry.automatic = job.automatic;
  entry.actor = FormDesc::FromString(job.actor).ToFormId(world.espmFiles);
  entry.command = job.command;
  entry.instance = job.instance;
  entry.contentMode = job.contentMode;
  entry.waitingExternal =
    job.Current().cursor == "waiting" && job.Current().awaitKind == "bool";
  entry.owner = world.LookupFormByIdNoLoad(ownerId);
  entries.insert_or_assign(job.id, std::move(entry));
}

void QuestJobRunner::OnActorLoaded(MpActor& actor) noexcept
{
  if (stopped)
    return;
  try {
    const auto jobs = QuestJournal::PersonalJobState(actor);
    for (const auto& [id, job] : jobs.live)
      Track(actor, job);
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
  const auto ownerId = owner.GetFormId();
  Track(owner, job);
  const auto time = Now();
  if (stopped)
    throw std::runtime_error(
      "Quest coordinator was cleared while reading its clock");
  RequireContext(actor, job);
  // Clock callbacks may clear or advance work. Reacquire scheduler entries
  // and validate the saved token before installing a recovery anchor.
  const auto latest = QuestJournal::JobState(actor);
  const auto* live = latest.FindLive(job.id);
  if (!live || live->generation != job.generation ||
      live->Current().token != work.token)
    throw std::runtime_error("Quest timer changed while reading its clock");
  auto& entry = scheduled.at(ownerId).jobs.at(job.id);
  if (entry.generation != job.generation || entry.owner.lock().get() != &owner)
    throw std::runtime_error("Quest timer identity changed");
  if (!entry.timer || entry.timer->token != work.token) {
    const auto remaining = work.timer->dueUnixMs > time.unixMs
      ? std::min(work.timer->dueUnixMs - time.unixMs, work.timer->durationMs)
      : 0;
    if (remaining > std::numeric_limits<uint64_t>::max() - time.steadyMs)
      throw std::runtime_error("Quest steady deadline overflow");
    entry.timer = QuestJobTimerAnchor{ work.token, time.steadyMs + remaining };
  }
  return time.steadyMs >= entry.timer->dueSteadyMs;
}

void QuestJobRunner::Retry(MpActor& actor, const Request& command)
{
  if (command.sequence) {
    const auto observed = Inspect(actor, command);
    if (observed.status == Status::Completed ||
        observed.status == Status::Retired)
      return;
  }
  RequireReady(actor);
  const auto jobs = QuestJournal::JobState(actor);
  RequireRequest(actor, command, jobs);
  const auto* job = Find(jobs, command);
  if (!job || !jobs.FindLive(job->id))
    throw std::runtime_error("No active quest job to retry");
  RequireContext(actor, *job);
  auto& owner = QuestJournal::Owner(actor);
  Track(owner, *job);
  auto& entry = scheduled.at(owner.GetFormId()).jobs.at(job->id);
  entry.error.clear();
  entry.quarantined = false;
}

void QuestJobRunner::Pump() noexcept
{
  if (stopped || pumping || QuestJournal::HasActiveTransaction() ||
      scheduled.empty())
    return;
  BusyGuard guard(pumping);
  // One execution attempt per campaign per pass; round robin within a
  // campaign prevents a polling root from starving another ready root.
  const auto candidates = std::min<size_t>(64, scheduled.size());
  std::array<uint32_t, 64> owners{};
  auto next = scheduled.upper_bound(lastPumpedOwner);
  for (size_t index = 0; index < candidates; ++index) {
    if (next == scheduled.end())
      next = scheduled.begin();
    owners[index] = next->first;
    ++next;
  }
  const auto blocked = [&](uint32_t ownerId) {
    const auto barrier = barriers.find(ownerId);
    return barrier != barriers.end() &&
      (barrier->second.busy ||
       (barrier->second.receipt &&
        barrier->second.receipt->GetState() != SaveReceipt::State::Durable));
  };
  size_t executed = 0;
  for (size_t index = 0; index < candidates && executed < 8 && !stopped;
       ++index) {
    const auto ownerId = owners[index];
    auto campaign = scheduled.find(ownerId);
    if (campaign == scheduled.end())
      continue;
    lastPumpedOwner = ownerId;
    if (blocked(ownerId))
      continue;
    std::array<uint64_t, QuestJobState::MaxLive> ids{};
    const auto count = std::min(ids.size(), campaign->second.jobs.size());
    auto nextJob =
      campaign->second.jobs.upper_bound(campaign->second.lastPumpedJob);
    for (size_t i = 0; i < count; ++i) {
      if (nextJob == campaign->second.jobs.end())
        nextJob = campaign->second.jobs.begin();
      ids[i] = nextJob->first;
      ++nextJob;
    }
    for (size_t i = 0; i < count && !stopped; ++i) {
      campaign = scheduled.find(ownerId);
      if (campaign == scheduled.end())
        break;
      const auto entry = campaign->second.jobs.find(ids[i]);
      if (entry == campaign->second.jobs.end())
        continue;
      campaign->second.lastPumpedJob = ids[i];
      if (!entry->second.automatic || entry->second.quarantined ||
          entry->second.waitingExternal ||
          (entry->second.contentMode == "fixture" && !schedulerFixture))
        continue;
      const auto jobId = entry->first;
      const auto generation = entry->second.generation;
      const auto expectedOwner = entry->second.owner;
      const auto quarantine = [&](const char* reason) noexcept {
        if (stopped)
          return;
        const auto ownerWork = scheduled.find(ownerId);
        if (ownerWork == scheduled.end())
          return;
        const auto current = ownerWork->second.jobs.find(jobId);
        if (current == ownerWork->second.jobs.end() ||
            current->second.generation != generation ||
            current->second.owner.lock().get() != expectedOwner.lock().get())
          return;
        current->second.quarantined = true;
        try {
          current->second.error = std::string(reason).substr(0, 1024);
          spdlog::error("Quest job {} for owner {:x} quarantined: {}", jobId,
                        ownerId, current->second.error);
        } catch (...) {
        }
      };
      bool attempted = false;
      try {
        // No scheduler references survive a clock or host callback.
        const auto work = entry->second;
        if (work.timer) {
          const auto time = Now();
          if (stopped)
            return;
          if (time.steadyMs < work.timer->dueSteadyMs)
            continue;
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
        const auto* job = jobs.FindLive(work.job);
        if (!job || job->generation != work.generation ||
            job->sequenced != work.sequenced || !job->automatic) {
          scheduled.at(ownerId).jobs.erase(jobId);
          continue;
        }
        try {
          RequireContext(*actor, *job);
        } catch (const std::exception&) {
          continue;
        }
        const auto request = RequestFor(*job);
        const auto observed = Inspect(*actor, request);
        if (blocked(ownerId))
          break;
        if (observed.status != Status::Ready)
          continue;
        RequireReady(*actor);
        ++executed;
        attempted = true;
        if (job->contentMode == "fixture")
          Advance(*actor, request, schedulerFixture->metadata,
                  schedulerFixture->scripts);
        else
          Advance(*actor, request);
      } catch (const std::exception& error) {
        quarantine(error.what());
      } catch (...) {
        quarantine("Unknown quest scheduler failure");
      }
      if (attempted || blocked(ownerId))
        break;
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
                                               const Request& command)
{
  if (stopped)
    return {
      Status::Failed, 0, false, {}, {}, "Quest coordinator was cleared"
    };
  const auto jobs = QuestJournal::JobState(actor);
  if (const auto retired = Retired(actor, command, jobs))
    return *retired;
  const auto* job = Find(jobs, command);
  if (!job)
    throw std::invalid_argument("Unknown quest job command");
  RequireContext(actor, *job);
  auto result = [&]() -> Result {
    const auto& owner = QuestJournal::Owner(actor);
    if (const auto found = barriers.find(owner.GetFormId());
        found != barriers.end()) {
      const auto& barrier = found->second;
      if (barrier.owner.lock().get() != &owner)
        throw std::runtime_error("Quest job owner was replaced");
      if (barrier.job <= job->id && job->id <= barrier.lastJob &&
          barrier.generation == job->generation &&
          (barrier.busy ||
           (barrier.receipt &&
            barrier.receipt->GetState() == SaveReceipt::State::Pending)))
        return { Status::Pending, job->id };
      if (barrier.job <= job->id && job->id <= barrier.lastJob &&
          barrier.generation == job->generation && barrier.receipt &&
          barrier.receipt->GetState() == SaveReceipt::State::Abandoned)
        return {
          Status::Failed, job->id, false, {}, {}, "Quest save was abandoned"
        };
    }
    const auto& current = job->Current();
    if (const auto campaign = scheduled.find(owner.GetFormId());
        campaign != scheduled.end()) {
      const auto scheduledJob = campaign->second.jobs.find(job->id);
      if (scheduledJob != campaign->second.jobs.end() &&
          scheduledJob->second.generation == job->generation &&
          scheduledJob->second.quarantined)
        return { Status::Failed,
                 job->id,
                 false,
                 {},
                 {},
                 scheduledJob->second.error.empty()
                   ? "Quest scheduler quarantined this job"
                   : scheduledJob->second.error };
    }
    if (!current.failure.empty())
      return { Status::Failed, job->id, false, {}, {}, current.failure };
    if (job->IsComplete())
      return { Status::Completed, job->id, job->accepted };
    if (current.cursor == "waiting" && current.awaitKind == "timer")
      return { TimerReady(actor, *job) ? Status::Ready : Status::Sleeping,
               job->id };
    if (current.cursor == "waiting" && !current.child)
      return { Status::Waiting, job->id, job->accepted, current.token,
               current.awaitKey };
    return { Status::Ready, job->id };
  }();
  switch (job->kind) {
    case QuestJobState::Kind::Stage:
      result.kind = ResultKind::Stage;
      break;
    case QuestJobState::Kind::Interaction:
      result.kind = ResultKind::Interaction;
      break;
    case QuestJobState::Kind::ReferenceLoad:
      result.kind = ResultKind::ReferenceLoad;
      break;
  }
  return result;
}

QuestJobRunner::Result QuestJobRunner::Submit(MpActor& actor,
                                              const Request& command,
                                              uint32_t quest, uint16_t stage)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto& pinned = Scripts();
  return SubmitWithContent(actor, command, quest, stage, pinned, {}, manifest,
                           false, true);
}

QuestJobRunner::Result QuestJobRunner::Submit(
  MpActor& actor, const Request& command, uint32_t quest, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  return Submit(actor, command, quest, stage,
                MetadataOverrides{ { quest, metadata } }, scripts);
}

QuestJobRunner::Result QuestJobRunner::Submit(
  MpActor& actor, const Request& command, uint32_t quest, uint16_t stage,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts, bool automatic)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return SubmitWithContent(actor, command, quest, stage, scripts, overrides,
                           hashManifest, true, automatic);
}

QuestJobRunner::Result QuestJobRunner::SubmitWithContent(
  MpActor& actor, const Request& command, uint32_t quest, uint16_t stage,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait, bool automatic)
{
  QuestJobState::Job job;
  job.quest = FormDesc::FromFormId(quest, world.espmFiles).ToString();
  job.stage = stage;
  job.contentHash = QuestJobContent::Hash(
    world, quest, Metadata(world, quest, overrides), hashManifest);
  return AdmitWithContent(actor, command, std::move(job), sources, overrides,
                          hashManifest, fixtureAwait, automatic);
}

QuestJobRunner::Result QuestJobRunner::SubmitEvent(MpActor& actor,
                                                   const Request& command,
                                                   MpObjectReference& target,
                                                   const std::string& name,
                                                   MpObjectReference& source)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  QuestJobState::Job job;
  job.kind = QuestJobState::Kind::Interaction;
  job.cursor = "done";
  job.interactionDone = false;
  job.interaction =
    QuestScriptRuntime::CaptureInteraction(actor, target, name, source);
  const auto& pinned = Scripts();
  return AdmitWithContent(actor, command, std::move(job), pinned, {}, manifest,
                          false, true);
}

QuestJobRunner::Result QuestJobRunner::SubmitEvent(
  MpActor& actor, const Request& command, MpObjectReference& target,
  const std::string& name, MpObjectReference& source,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources, bool automatic)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  QuestJobState::Job job;
  job.kind = QuestJobState::Kind::Interaction;
  job.cursor = "done";
  job.interactionDone = false;
  job.interaction =
    QuestScriptRuntime::CaptureInteraction(actor, target, name, source);
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(sources), overrides);
  return AdmitWithContent(actor, command, std::move(job), sources, overrides,
                          hashManifest, true, automatic);
}

QuestJobRunner::Result QuestJobRunner::SubmitLoad(MpActor& actor,
                                                  const Request& command,
                                                  MpObjectReference& target)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  QuestJobState::Job job;
  job.kind = QuestJobState::Kind::ReferenceLoad;
  job.cursor = "done";
  job.loadPhase = QuestJobState::LoadPhase::Init;
  job.referenceLoad = QuestScriptRuntime::CaptureReferenceLoad(actor, target);
  const auto& pinned = Scripts();
  return AdmitWithContent(actor, command, std::move(job), pinned, {}, manifest,
                          false, true);
}

QuestJobRunner::Result QuestJobRunner::SubmitLoad(
  MpActor& actor, const Request& command, MpObjectReference& target,
  const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& sources, bool automatic)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  QuestJobState::Job job;
  job.kind = QuestJobState::Kind::ReferenceLoad;
  job.cursor = "done";
  job.loadPhase = QuestJobState::LoadPhase::Init;
  job.referenceLoad = QuestScriptRuntime::CaptureReferenceLoad(actor, target);
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(sources), overrides);
  return AdmitWithContent(actor, command, std::move(job), sources, overrides,
                          hashManifest, true, automatic);
}

QuestJobRunner::Result QuestJobRunner::AdmitWithContent(
  MpActor& actor, const Request& command, QuestJobState::Job job,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait, bool automatic, const CapturedOccurrence* occurrence)
{
  if (command.command.empty() || command.command.size() > 128 ||
      command.command.find('\0') != std::string::npos || stopped ||
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
  RequireRequest(actor, command, jobs);
  if (job.occurrence) {
    if (!occurrence || *job.occurrence != occurrence->id.sequence ||
        occurrence->actor != actor.GetChangeForm().formDesc.ToString() ||
        occurrence->instance != actor.GetWorldInstance() ||
        job.kind != QuestJobState::Kind::Interaction ||
        job.interaction != occurrence->interaction)
      throw std::runtime_error("Native occurrence lost its immutable capture");
    RequireOccurrence(actor, occurrence->id, jobs);
  }
  if (const auto* prior = Find(jobs, command)) {
    RequireContext(actor, *prior);
    RequireMode(*prior, fixtureAwait);
    if (prior->kind != job.kind || prior->occurrence != job.occurrence ||
        prior->interaction != job.interaction ||
        prior->referenceLoad != job.referenceLoad ||
        prior->quest != job.quest || prior->stage != job.stage ||
        prior->contentHash != job.contentHash)
      throw std::runtime_error("Quest command was reused for different work");
    ValidateContent(*prior, overrides, hashManifest);
    return Inspect(actor, command);
  }
  RequireReady(actor);
  for (const auto& [id, live] : jobs.live) {
    if (job.referenceLoad && live.referenceLoad &&
        job.referenceLoad->target == live.referenceLoad->target)
      throw std::runtime_error("Reference already has a pending load job");
  }
  const auto legacyCount =
    std::count_if(jobs.completed.begin(), jobs.completed.end(),
                  [](const auto& job) { return !job.sequenced; }) +
    std::count_if(jobs.live.begin(), jobs.live.end(),
                  [](const auto& entry) { return !entry.second.sequenced; });
  const auto stageCount =
    std::count_if(jobs.live.begin(), jobs.live.end(), [](const auto& entry) {
      return entry.second.kind == QuestJobState::Kind::Stage;
    });
  if (jobs.live.size() >= QuestJobState::MaxLive ||
      (job.kind == QuestJobState::Kind::Stage &&
       stageCount >= QuestJobState::MaxStageLive) ||
      (!command.sequence && legacyCount >= QuestJobState::MaxCompleted) ||
      jobs.nextJob >= MaxIdentity)
    throw std::runtime_error(
      "Campaign has active work or its command history is full");
  if (command.sequence && command.sequence->sequence != jobs.nextJob)
    throw std::runtime_error(
      "Quest request is not the next admission identity");
  const auto environment = QuestJobContent::Environment(world, hashManifest);
  for (const auto& [id, live] : jobs.live)
    if (live.environmentHash.empty() || live.environmentHash != environment ||
        live.contentMode != (fixtureAwait ? "fixture" : "owned"))
      throw std::runtime_error(
        "Concurrent quest jobs require one pinned environment");
  auto retiredIds =
    command.sequence ? jobs.RetireForAdmission() : std::vector<uint64_t>{};
  if (job.occurrence) {
    if (!command.sequence || *job.occurrence != jobs.ingress.next ||
        jobs.ingress.next >= MaxIdentity)
      throw std::runtime_error(
        "Native occurrence sequence changed before admission");
    jobs.ingress.identity = occurrence->id.campaignIdentity;
    ++jobs.ingress.next;
  }
  if (job.kind == QuestJobState::Kind::Stage)
    QuestJournal::RequireAdapter(
      actor, FormDesc::FromString(job.quest).ToFormId(world.espmFiles));
  job.id = jobs.nextJob++;
  job.generation = jobs.generation;
  job.command = command.command;
  job.sequenced = command.sequence.has_value();
  job.automatic = automatic;
  job.contentMode = fixtureAwait ? "fixture" : "owned";
  job.environmentHash = environment;
  job.actor =
    FormDesc::FromFormId(actor.GetFormId(), world.espmFiles).ToString();
  job.owner =
    FormDesc::FromFormId(owner.GetFormId(), world.espmFiles).ToString();
  job.instance = actor.GetWorldInstance();
  RequireContext(actor, job);
  const auto jobId = job.id;
  jobs.live.emplace(jobId, std::move(job));
  jobs.Save();
  return RunSegment(actor, std::move(jobs), jobId, sources, overrides,
                    hashManifest, {}, fixtureAwait, true, false,
                    std::move(retiredIds));
}

void QuestJobRunner::ValidateContent(const QuestJobState::Job& job,
                                     const MetadataOverrides& overrides,
                                     const std::string& hashManifest) const
{
  if (!job.environmentHash.empty() &&
      job.environmentHash != QuestJobContent::Environment(world, hashManifest))
    throw std::runtime_error("Quest shared environment changed");
  const auto check = [&](const QuestJobState::Stage& work) {
    const auto quest =
      FormDesc::FromString(work.quest).ToFormId(world.espmFiles);
    if (work.contentHash !=
        QuestJobContent::Hash(world, quest, Metadata(world, quest, overrides),
                              hashManifest))
      throw std::runtime_error("Quest job content changed");
  };
  if (job.kind == QuestJobState::Kind::Stage)
    check(job);
  for (const auto& child : job.children)
    check(child);
}

QuestJobRunner::Result QuestJobRunner::Resume(MpActor& actor,
                                              const Request& command,
                                              const std::string& token,
                                              bool completion)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto& pinned = Scripts();
  return ResumeWithContent(actor, command, token, completion, pinned, {},
                           manifest, false);
}

QuestJobRunner::Result QuestJobRunner::Resume(
  MpActor& actor, const Request& command, const std::string& token,
  bool completion, const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  const auto jobs = QuestJournal::JobState(actor);
  if (const auto retired = Retired(actor, command, jobs))
    return *retired;
  const auto* job = Find(jobs, command);
  if (!job)
    throw std::invalid_argument("Unknown quest job command");
  if (job->kind != QuestJobState::Kind::Stage)
    throw std::invalid_argument(
      "Reference jobs require a complete metadata override map");
  const auto quest =
    FormDesc::FromString(job->quest).ToFormId(world.espmFiles);
  return Resume(actor, command, token, completion,
                MetadataOverrides{ { quest, metadata } }, scripts);
}

QuestJobRunner::Result QuestJobRunner::Resume(
  MpActor& actor, const Request& command, const std::string& token,
  bool completion, const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return ResumeWithContent(actor, command, token, completion, scripts,
                           overrides, hashManifest, true);
}

QuestJobRunner::Result QuestJobRunner::ResumeWithContent(
  MpActor& actor, const Request& command, const std::string& token,
  bool completion, const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait)
{
  auto jobs = QuestJournal::JobState(actor);
  RequireRequest(actor, command, jobs);
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
  const auto* live = jobs.FindLive(prior->id);
  if (!live)
    throw std::runtime_error("Quest completion does not match active work");
  const auto& current = live->Current();
  if (current.cursor != "waiting" || current.awaitKind != "bool" ||
      current.child || current.token != token || !current.failure.empty())
    throw std::runtime_error("Quest completion does not match external work");
  const auto jobId = prior->id;
  return RunSegment(actor, std::move(jobs), jobId, sources, overrides,
                    hashManifest, completion, fixtureAwait, false);
}

QuestJobRunner::Result QuestJobRunner::Advance(MpActor& actor,
                                               const Request& command)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto& pinned = Scripts();
  return AdvanceWithContent(actor, command, pinned, {}, manifest, false);
}

QuestJobRunner::Result QuestJobRunner::Advance(
  MpActor& actor, const Request& command, const MetadataOverrides& overrides,
  const std::vector<PexScript::Lazy>& scripts)
{
  if (const auto retired = Retired(actor, command))
    return *retired;
  const auto hashManifest = QuestJobContent::WithMetadataOverrides(
    world, QuestJobContent::ScriptManifest(scripts), overrides);
  return AdvanceWithContent(actor, command, scripts, overrides, hashManifest,
                            true);
}

QuestJobRunner::Result QuestJobRunner::AdvanceWithContent(
  MpActor& actor, const Request& command,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  bool fixtureAwait)
{
  auto jobs = QuestJournal::JobState(actor);
  RequireRequest(actor, command, jobs);
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
  // Inspect may call an injected clock. Resume from fresh committed state.
  jobs = QuestJournal::JobState(actor);
  prior = Find(jobs, command);
  if (!prior || !jobs.FindLive(prior->id))
    throw std::runtime_error("Quest work changed during inspection");
  RequireContext(actor, *prior);
  RequireMode(*prior, fixtureAwait);
  ValidateContent(*prior, overrides, hashManifest);
  const auto* live = jobs.FindLive(prior->id);
  const auto& current = live->Current();
  const bool consumeChild = current.node && current.cursor == "done";
  const bool timer =
    current.cursor == "waiting" && current.awaitKind == "timer";
  const bool hostWork = live->cursor == "done" && live->children.empty() &&
    current.cursor == "done";
  if (current.cursor != "begin" && !consumeChild && !timer && !hostWork)
    throw std::runtime_error("Quest job has no internal work ready");
  const auto jobId = prior->id;
  return RunSegment(
    actor, std::move(jobs), jobId, sources, overrides, hashManifest,
    timer ? std::optional(QuestJobState::Completion::None()) : std::nullopt,
    fixtureAwait, false, consumeChild);
}

QuestJobRunner::Result QuestJobRunner::RunSegment(
  MpActor& actor, QuestJobState jobs, uint64_t jobId,
  const std::vector<PexScript::Lazy>& sources,
  const MetadataOverrides& overrides, const std::string& hashManifest,
  std::optional<QuestJobState::Completion> completion, bool fixtureAwait,
  bool submit, bool consumeChild, std::vector<uint64_t> retiredIds)
{
  auto& owner = QuestJournal::Owner(actor);
  auto& job = jobs.live.at(jobId);
  const auto admittedOccurrence = submit ? job.occurrence : std::nullopt;
  const auto admissionGeneration = job.generation;
  const auto ownerId = owner.GetFormId();
  const auto command = RequestFor(job);
  if (!barriers.contains(ownerId) && barriers.size() >= 4096)
    throw std::runtime_error("Quest coordinator campaign limit exceeded");
  Barrier initialBarrier;
  initialBarrier.owner = world.LookupFormByIdNoLoad(ownerId);
  auto& barrier =
    barriers.try_emplace(ownerId, std::move(initialBarrier)).first->second;
  Track(owner, job);
  barrier.job = barrier.lastJob = job.id;
  barrier.generation = job.generation;
  barrier.owner = world.LookupFormByIdNoLoad(ownerId);
  try {
    BusyGuard guard(barrier.busy);
    struct AdmissionGuard
    {
      Barrier& barrier;
      AdmissionGuard(Barrier& value, std::optional<uint64_t> occurrence,
                     uint64_t generation)
        : barrier(value)
      {
        if (occurrence) {
          barrier.admittingOccurrence = barrier.admittingLastOccurrence =
            *occurrence;
          barrier.admittingGeneration = generation;
        }
      }
      ~AdmissionGuard()
      {
        barrier.admittingOccurrence = barrier.admittingLastOccurrence = 0;
        barrier.admittingGeneration = 0;
      }
    } admissionGuard(barrier, admittedOccurrence, admissionGeneration);
    std::optional<QuestJobTimerAnchor> timerAnchor;
    const auto receipt = QuestJournal::AtomicJob(actor, jobId, [&] {
      const auto requireAdapter = [&](const QuestJobState::Stage& stage) {
        QuestJournal::RequireAdapter(
          actor, FormDesc::FromString(stage.quest).ToFormId(world.espmFiles));
      };
      if (job.kind == QuestJobState::Kind::Stage)
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
      espm::QuestScriptData metadata;
      if (!job.Current().quest.empty()) {
        const auto quest =
          FormDesc::FromString(job.Current().quest).ToFormId(world.espmFiles);
        QuestJournal::RequireAdapter(actor, quest);
        metadata = Metadata(world, quest, overrides);
      }
      QuestScriptRuntime::ExecuteJobSegment(
        actor, job, metadata, sources, completion, fixtureAwait, overrides,
        hashManifest, [this] { return Now(); }, &timerAnchor);
      ++job.segment;
      if (job.IsComplete()) {
        auto retired = jobs.CompleteJob(jobId);
        retiredIds.insert(retiredIds.end(), retired.begin(), retired.end());
      }
      QuestJournal::SetJobState(actor, jobs);
    });
    // All post-commit publication below is nonallocating.
    barrier.receipt = receipt;
    if (admittedOccurrence) {
      barrier.occurrence = barrier.lastOccurrence = *admittedOccurrence;
      barrier.occurrenceGeneration = admissionGeneration;
      barrier.admissionReceipt = receipt;
    }
    if (!retiredIds.empty()) {
      barrier.retiredIds = std::move(retiredIds);
      barrier.retirementGeneration = jobs.generation;
      barrier.retirementReceipt = receipt;
    }
    const auto campaign = scheduled.find(ownerId);
    if (campaign != scheduled.end()) {
      const auto entry = campaign->second.jobs.find(jobId);
      if (entry != campaign->second.jobs.end()) {
        if (const auto* live = jobs.FindLive(jobId)) {
          entry->second.timer = std::move(timerAnchor);
          entry->second.waitingExternal =
            live->Current().cursor == "waiting" &&
            live->Current().awaitKind == "bool";
        } else
          campaign->second.jobs.erase(entry);
      }
    }
  } catch (...) {
    // An admission that rolled back must not consume a live scheduler slot.
    if (submit) {
      const auto campaign = scheduled.find(ownerId);
      if (campaign != scheduled.end())
        campaign->second.jobs.erase(jobId);
    }
    throw;
  }
  return Inspect(actor, command);
}
