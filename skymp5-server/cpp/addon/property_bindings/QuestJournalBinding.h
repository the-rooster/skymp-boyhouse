#pragma once
#include "PropertyBinding.h"

class QuestJournalBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "questJournal"; }
  Napi::Value Get(Napi::Env env, ScampServer& server, uint32_t id) override;
};
class QuestCommandBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "questCommand"; }
  void Set(Napi::Env env, ScampServer& server, uint32_t id,
           Napi::Value command) override;
};

class QuestStageExecutionBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "questExecuteStage"; }
  void Set(Napi::Env env, ScampServer& server, uint32_t id,
           Napi::Value command) override;
};
