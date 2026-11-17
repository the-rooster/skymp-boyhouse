#pragma once
#include "MpChangeForms.h"
#include <chrono>
#include <optional>

class MpObjectReference;

namespace ChangeFormGuard_ {
void RequestSave(MpObjectReference* self);
}

class ChangeFormGuard
{
public:
  ChangeFormGuard(const MpChangeForm& changeForm_, MpObjectReference* self_)
    : changeForm(changeForm_)
    , self(self_)
  {
  }

  enum class Mode
  {
    RequestSave,
    NoRequestSave
  };

  void EditChangeForm(std::function<void(MpChangeForm&)> f,
                      Mode mode = Mode::RequestSave)
  {
    f(changeForm);
    if (!blockSaving && mode == Mode::RequestSave) {
      lastSaveRequest = std::chrono::system_clock::now();
      ChangeFormGuard_::RequestSave(self);
    }
  }

  const MpChangeForm& ChangeForm() const noexcept { return changeForm; }

  // The world transaction has already prepared its save-buffer copy. No
  // allocation, callback, event or save request may interrupt publication.
  void SwapPreparedCampaignFields(MpChangeForm& prepared) noexcept
  {
    static_assert(noexcept(changeForm.inv.entries.swap(prepared.inv.entries)));
    changeForm.inv.entries.swap(prepared.inv.entries);
    changeForm.dynamicFields.Swap(prepared.dynamicFields);
    static_assert(noexcept(changeForm.factions.swap(prepared.factions)));
    changeForm.factions.swap(prepared.factions);
    changeForm.baseContainerAdded = prepared.baseContainerAdded;
    changeForm.isDisabled = prepared.isDisabled;
    lastSaveRequest = std::chrono::system_clock::now();
  }

  // The movement transaction already owns its save snapshot and occupancy.
  // Do not call SetPos: its subscription and trigger tails are not atomic.
  void CommitPreparedMovementTransform(const NiPoint3& position,
                                       const NiPoint3& angle) noexcept
  {
    static_assert(noexcept(changeForm.position = position));
    static_assert(noexcept(changeForm.angle = angle));
    changeForm.position = position;
    changeForm.angle = angle;
  }

  auto GetLastSaveRequestMoment() const { return lastSaveRequest; }

  bool blockSaving = false;

private:
  MpChangeForm changeForm;
  MpObjectReference* const self;
  std::optional<std::chrono::system_clock::time_point> lastSaveRequest;
};
