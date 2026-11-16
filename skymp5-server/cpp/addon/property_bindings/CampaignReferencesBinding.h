#pragma once
#include "PropertyBinding.h"

class CampaignReferencesBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "campaignReferences"; }
  void Set(Napi::Env env, ScampServer& server, uint32_t id,
           Napi::Value command) override;
};
