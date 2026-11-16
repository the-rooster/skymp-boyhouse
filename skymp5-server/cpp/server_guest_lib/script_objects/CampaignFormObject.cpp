#include "CampaignFormObject.h"
#include <stdexcept>

CampaignFormObject::CampaignFormObject(const espm::LookupResult& record,
                                       uint32_t owner, uint32_t id)
  : EspmGameObject(record)
  , ownerId(owner)
  , formId(id)
{
  if (!owner || !id ||
      (id != 0x14 &&
       (!record.rec || record.ToGlobalId(record.rec->GetId()) != id)))
    throw std::invalid_argument("Invalid campaign form identity");
  if (record.rec &&
      (record.rec->GetType() == "QUST" || record.rec->GetType() == "GLOB"))
    throw std::invalid_argument("Quest/global needs its campaign object type");
  identity =
    "campaign:" + std::to_string(owner) + ":form:" + std::to_string(id);
}
const char* CampaignFormObject::GetParentNativeScript()
{
  return IsPlayer() ? "actor" : EspmGameObject::GetParentNativeScript();
}
bool CampaignFormObject::EqualsByValue(const IGameObject& object) const
{
  const auto* other = dynamic_cast<const CampaignFormObject*>(&object);
  return other && other->ownerId == ownerId && other->formId == formId;
}
