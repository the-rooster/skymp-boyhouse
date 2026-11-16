#include "QuestJournalBinding.h"
#include "NapiHelper.h"
#include "QuestJournal.h"

Napi::Value QuestJournalBinding::Get(Napi::Env env, ScampServer& server,
                                     uint32_t id)
{
  auto& actor = server.GetPartOne()->worldState.GetFormAt<MpActor>(id);
  return NapiHelper::ParseJson(env, QuestJournal::Snapshot(actor));
}

void QuestCommandBinding::Set(Napi::Env env, ScampServer& server, uint32_t id,
                              Napi::Value command)
{
  auto& actor = server.GetPartOne()->worldState.GetFormAt<MpActor>(id);
  QuestJournal::Apply(
    actor, nlohmann::json::parse(NapiHelper::Stringify(env, command)));
}
