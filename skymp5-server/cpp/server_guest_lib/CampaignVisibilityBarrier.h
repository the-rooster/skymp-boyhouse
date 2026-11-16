#pragma once
#include <Networking.h>
#include <functional>
#include <memory>
#include <vector>

class MpActor;
class PartOne;

// Synchronous, paired callbacks for WorldState's visibility commit boundary.
// BeforeCommit either closes every observer's native gate and obtains a fresh
// epoch receipt, or throws before publication. Every entered BeforeCommit must
// receive exactly one AfterCommit, including failures; false means abort OR
// postcommit desynchronization. Only the ordinary projection ack may reopen a
// successful connection after AfterCommit returns.
class CampaignVisibilityBarrier
{
public:
  using UserIds = std::vector<Networking::UserId>;
  using Before = std::function<bool(uint32_t, const UserIds&)>;
  using After = std::function<bool(uint32_t, const UserIds&, bool)>;
  using Close = std::function<void(Networking::UserId)>;

  CampaignVisibilityBarrier(PartOne&, Before, After, Close);
  ~CampaignVisibilityBarrier();
  void BeforeCommit(MpActor& owner);
  void AfterCommit(MpActor& owner, bool synchronized) noexcept;
  bool IsActive() const noexcept;
  bool HoldsGate(Networking::UserId user) const noexcept;

private:
  struct Context;
  void Validate(bool advanced);
  void Quarantine() noexcept;

  PartOne& server;
  Before before;
  After after;
  Close close;
  std::unique_ptr<Context> context;
  size_t rejectedNested = 0;
};
