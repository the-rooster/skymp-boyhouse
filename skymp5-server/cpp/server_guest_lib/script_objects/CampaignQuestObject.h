#pragma once
#include "EspmGameObject.h"

class CampaignQuestObject final : public EspmGameObject
{
public:
  CampaignQuestObject(const espm::LookupResult& record, uint32_t ownerId);
  uint32_t OwnerId() const { return ownerId; }
  const char* GetStringID() override { return identity.c_str(); }
  bool EqualsByValue(const IGameObject& object) const override;
  std::shared_ptr<ActivePexInstance> Script(const std::string& name) const;

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
  uint32_t ownerId;
  std::string identity;
  std::vector<std::shared_ptr<ActivePexInstance>> scripts;
};
