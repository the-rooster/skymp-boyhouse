#pragma once
#include "EspmGameObject.h"

// A logical campaign identity, not a live shared-world reference. Mutable
// world natives must use a transactional overlay rather than cast this to an
// ordinary MpObjectReference. PlayerRef is the campaign owner, never a guest.
class CampaignFormObject final : public EspmGameObject
{
public:
  CampaignFormObject(const espm::LookupResult& record, uint32_t owner,
                     uint32_t formId);
  uint32_t OwnerId() const { return ownerId; }
  uint32_t FormId() const { return formId; }
  bool IsPlayer() const { return formId == 0x14; }
  const char* GetStringID() override { return identity.c_str(); }
  const char* GetParentNativeScript() override;
  bool EqualsByValue(const IGameObject& object) const override;

private:
  uint32_t ownerId, formId;
  std::string identity;
};
