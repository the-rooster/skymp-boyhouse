#include "CampaignAliasObject.h"
#include <stdexcept>

CampaignAliasObject::CampaignAliasObject(uint32_t owner, uint32_t quest,
                                         espm::QuestAliasDefinition value)
  : ownerId(owner)
  , questId(quest)
  , definition(std::move(value))
{
  if (!owner || !quest)
    throw std::invalid_argument("Invalid campaign alias identity");
  identity = "campaign:" + std::to_string(owner) +
    ":quest:" + std::to_string(quest) +
    ":alias:" + std::to_string(definition.id);
}
const char* CampaignAliasObject::GetParentNativeScript()
{
  return definition.kind == espm::QuestAliasDefinition::Kind::Reference
    ? "referencealias"
    : "locationalias";
}
bool CampaignAliasObject::EqualsByValue(const IGameObject& object) const
{
  const auto* other = dynamic_cast<const CampaignAliasObject*>(&object);
  return other && other->ownerId == ownerId && other->questId == questId &&
    other->definition.id == definition.id &&
    other->definition.kind == definition.kind;
}
