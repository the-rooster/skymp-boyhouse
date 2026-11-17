#pragma once
#include "QuestJobClock.h"
#include "QuestJobState.h"
#include "SaveReceipt.h"
#include "libespm/QuestScriptData.h"
#include "papyrus-vm/PexScript.h"
#include <map>
#include <memory>

class MpActor;
class MpForm;
class WorldState;

// Trusted host coordinator. Waiting requests and completed results are only
// observable after their exact segment has committed to storage. No client
// packet can submit jobs or supply completions through this interface.
class QuestJobRunner
{
public:
  enum class Status
  {
    Pending,
    Ready,
    Waiting,
    Sleeping,
    Completed,
    Failed
  };
  using MetadataOverrides = std::map<uint32_t, espm::QuestScriptData>;
  struct Result
  {
    Status status = Status::Pending;
    uint64_t job = 0;
    bool accepted = false;
    std::string token, key, error;
  };
  Result Submit(MpActor& actor, const std::string& command, uint32_t quest,
                uint16_t stage);
  Result Submit(MpActor& actor, const std::string& command, uint32_t quest,
                uint16_t stage, const espm::QuestScriptData& metadata,
                const std::vector<PexScript::Lazy>& scripts);
  Result Submit(MpActor& actor, const std::string& command, uint32_t quest,
                uint16_t stage, const MetadataOverrides& overrides,
                const std::vector<PexScript::Lazy>& scripts,
                bool automatic = false);
  Result Resume(MpActor& actor, const std::string& command,
                const std::string& token, bool completion);
  Result Resume(MpActor& actor, const std::string& command,
                const std::string& token, bool completion,
                const espm::QuestScriptData& metadata,
                const std::vector<PexScript::Lazy>& scripts);
  Result Resume(MpActor& actor, const std::string& command,
                const std::string& token, bool completion,
                const MetadataOverrides& overrides,
                const std::vector<PexScript::Lazy>& scripts);
  // Advance one internally scheduled segment after its predecessor is durable.
  // Resume cannot complete pending internal work. Matching already-consumed
  // tokens are harmless queries, regardless of their original source.
  Result Advance(MpActor& actor, const std::string& command);
  Result Advance(MpActor& actor, const std::string& command,
                 const MetadataOverrides& overrides,
                 const std::vector<PexScript::Lazy>& scripts);
  Result Inspect(MpActor& actor, const std::string& command);
  void Retry(MpActor& actor, const std::string& command);
  // Trusted dependency injection. Changing an armed live clock is refused.
  void SetClock(QuestJobClock clock);
  void SetSchedulerFixtureContent(const MetadataOverrides& overrides,
                                  const std::vector<PexScript::Lazy>& scripts);

private:
  friend class WorldState;
  explicit QuestJobRunner(WorldState& world);
  void Stop() noexcept;
  void Pump() noexcept;
  void OnActorLoaded(MpActor& actor) noexcept;
  struct Barrier
  {
    uint64_t job = 0;
    std::weak_ptr<MpForm> owner;
    std::shared_ptr<const SaveReceipt> receipt;
    bool busy = false;
    std::optional<QuestJobTimerAnchor> timer;
  };
  struct Scheduled
  {
    uint64_t job = 0;
    uint32_t actor = 0;
    std::string command, error, instance, contentMode;
    bool waitingExternal = false, quarantined = false;
    std::weak_ptr<MpForm> owner;
  };
  struct FixtureContent
  {
    MetadataOverrides metadata;
    std::vector<PexScript::Lazy> scripts;
  };
  QuestJobTime Now();
  bool TimerReady(MpActor&, const QuestJobState::Job&);
  void Track(MpActor& owner, const QuestJobState::Job& job);
  void RequireContext(MpActor& actor, const QuestJobState::Job& job) const;
  void RequireReady(MpActor& actor);
  const std::vector<PexScript::Lazy>& Scripts();
  Result SubmitWithContent(MpActor&, const std::string&, uint32_t, uint16_t,
                           const std::vector<PexScript::Lazy>&,
                           const MetadataOverrides&, const std::string&,
                           bool fixtureAwait, bool automatic);
  Result ResumeWithContent(MpActor&, const std::string&, const std::string&,
                           bool, const std::vector<PexScript::Lazy>&,
                           const MetadataOverrides&, const std::string&,
                           bool fixtureAwait);
  Result AdvanceWithContent(MpActor&, const std::string&,
                            const std::vector<PexScript::Lazy>&,
                            const MetadataOverrides&, const std::string&,
                            bool fixtureAwait);
  void ValidateContent(const QuestJobState::Job&, const MetadataOverrides&,
                       const std::string&) const;
  Result RunSegment(MpActor&, QuestJobState,
                    const std::vector<PexScript::Lazy>&,
                    const MetadataOverrides&, const std::string&,
                    std::optional<QuestJobState::Completion>,
                    bool fixtureAwait, bool submit, bool consumeChild = false);
  WorldState& world;
  bool stopped = false;
  bool pumping = false;
  uint32_t lastPumpedOwner = 0;
  QuestJobClock clock;
  std::optional<uint64_t> lastSteadyMs;
  std::optional<FixtureContent> schedulerFixture;
  std::map<uint32_t, Scheduled> scheduled;
  std::map<uint32_t, Barrier> barriers;
  std::vector<PexScript::Lazy> scripts;
  std::string manifest;
};
