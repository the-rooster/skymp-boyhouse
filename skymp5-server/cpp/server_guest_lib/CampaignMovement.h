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

// One prevalidated same-cell, same-grid physical sample. The world seals its
// source baseline and all resulting occurrences before publishing this plan.
class CampaignMovementPlan
{
public:
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
  struct Impl;
  explicit CampaignMovementPlan(std::unique_ptr<Impl> impl);
  void SetAuthorityValidator(std::function<bool()> validator);
  void Publish(std::shared_ptr<const SaveReceipt> receipt) noexcept;
  std::unique_ptr<Impl> pImpl;
};
