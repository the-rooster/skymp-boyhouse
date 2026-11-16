#include "CampaignQuestObject.h"
#include "papyrus-vm/Utils.h"

CampaignQuestObject::CampaignQuestObject(const espm::LookupResult& record,
                                         uint32_t ownerId_)
  : EspmGameObject(record)
  , ownerId(ownerId_)
{
  if (!record.rec || record.rec->GetType() != "QUST" || !ownerId)
    throw std::invalid_argument("Invalid campaign quest object");
  identity = "campaign:" + std::to_string(ownerId) +
    ":quest:" + std::to_string(record.ToGlobalId(record.rec->GetId()));
}

bool CampaignQuestObject::EqualsByValue(const IGameObject& object) const
{
  const auto* other = dynamic_cast<const CampaignQuestObject*>(&object);
  return other && other->ownerId == ownerId && other->record.rec == record.rec;
}

std::shared_ptr<ActivePexInstance> CampaignQuestObject::Script(
  const std::string& name) const
{
  for (const auto& script : scripts)
    if (!Utils::stricmp(script->GetSourcePexName().c_str(), name.c_str()))
      return script;
  throw std::runtime_error("Quest script is not attached: " + name);
}
