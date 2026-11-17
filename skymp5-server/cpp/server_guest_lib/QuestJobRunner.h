#pragma once
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
    Waiting,
    Completed,
    Failed
  };
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
  Result Resume(MpActor& actor, const std::string& command,
                const std::string& token, bool completion);
  Result Resume(MpActor& actor, const std::string& command,
                const std::string& token, bool completion,
                const espm::QuestScriptData& metadata,
                const std::vector<PexScript::Lazy>& scripts);
  Result Inspect(MpActor& actor, const std::string& command);

private:
  friend class WorldState;
  explicit QuestJobRunner(WorldState& world);
  void Stop() noexcept;
  struct Barrier
  {
    uint64_t job = 0;
    std::weak_ptr<MpForm> owner;
    std::shared_ptr<const SaveReceipt> receipt;
    bool busy = false;
  };
  void RequireContext(MpActor& actor, const QuestJobState::Job& job) const;
  void RequireReady(MpActor& actor);
  const std::vector<PexScript::Lazy>& Scripts();
  Result SubmitWithContent(MpActor&, const std::string&, uint32_t, uint16_t,
                           const espm::QuestScriptData&,
                           const std::vector<PexScript::Lazy>&,
                           const std::string&, bool fixtureAwait);
  Result ResumeWithContent(MpActor&, const std::string&, const std::string&,
                           bool, const espm::QuestScriptData&,
                           const std::vector<PexScript::Lazy>&,
                           const std::string&, bool fixtureAwait);
  WorldState& world;
  bool stopped = false;
  std::map<uint32_t, Barrier> barriers;
  std::vector<PexScript::Lazy> scripts;
  std::string manifest;
};
