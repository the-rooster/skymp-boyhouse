#pragma once
#include "libespm/QuestAliasData.h"
#include "papyrus-vm/VirtualMachine.h"

class CampaignAliasObject final : public IGameObject
{
public:
  CampaignAliasObject(uint32_t owner, uint32_t quest,
                      espm::QuestAliasDefinition definition);
  uint32_t OwnerId() const { return ownerId; }
  uint32_t QuestId() const { return questId; }
  const espm::QuestAliasDefinition& Definition() const { return definition; }
  const char* GetStringID() override { return identity.c_str(); }
  const char* GetParentNativeScript() override;
  bool EqualsByValue(const IGameObject& object) const override;

protected:
  const std::vector<std::shared_ptr<ActivePexInstance>>&
  ListActivePexInstances() const override
  {
    return scripts;
  }
  void AddScript(std::shared_ptr<ActivePexInstance> script) noexcept override
  {
    scripts.push_back(std::move(script));
  }

private:
  uint32_t ownerId, questId;
  espm::QuestAliasDefinition definition;
  std::string identity;
  std::vector<std::shared_ptr<ActivePexInstance>> scripts;
};
