#pragma once
#include "NiPoint3.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class MpActor;
class MpForm;
class MpObjectReference;
class SaveReceipt;
class WorldState;
class ActionListener;
class PartOne;

// One prevalidated same-cell, same-grid physical sample. The world seals its
// source baseline and all resulting occurrences before publishing this plan.
class CampaignMovementPlan
{
public:
  enum class Kind
  {
    Movement,
    Resume,
    Detach
  };
  struct SubscriptionChange
  {
    std::shared_ptr<MpForm> emitter, listener;
    bool added = false;
  };
  Kind GetKind() const noexcept;
  const std::vector<SubscriptionChange>& GetSubscriptionChanges() const;
  // Checks the accepted topology after durability, without rerunning scripts
  // or comparing volatile actor values/presentation data during client ACK.
  void ValidateCommitted(MpActor& requester) const;
  static constexpr auto kProperty = "private.boyhouse.movementBaseline";
  struct Event
  {
    std::shared_ptr<MpForm> target;
    std::string name;
  };
  ~CampaignMovementPlan();
  const std::vector<Event>& GetEvents() const;
  MpActor& GetSource() const;
  const NiPoint3& GetPosition() const;
  const NiPoint3& GetAngle() const;
  const std::string& GetBaselineDump() const;
  void Validate(MpActor& requester) const;

private:
  friend class MpObjectReference;
  friend class WorldState;
  friend class ActionListener;
  friend class PartOne;
  struct Impl;
  explicit CampaignMovementPlan(std::unique_ptr<Impl> impl);
  void SetAuthorityValidator(std::function<bool()> validator);
  void Publish(std::shared_ptr<const SaveReceipt> receipt) noexcept;
  std::unique_ptr<Impl> pImpl;
};
