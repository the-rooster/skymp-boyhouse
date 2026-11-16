#include "QuestJournalBinding.h"
#include "NapiHelper.h"
#include "QuestJournal.h"
#include "QuestScriptRuntime.h"

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

void QuestStageExecutionBinding::Set(Napi::Env env, ScampServer& server,
                                     uint32_t id, Napi::Value command)
{
  auto& actor = server.GetPartOne()->worldState.GetFormAt<MpActor>(id);
  const auto parsed =
    nlohmann::json::parse(NapiHelper::Stringify(env, command));
  const auto& quest = parsed.at("quest");
  const auto& stage = parsed.at("stage");
  if (!quest.is_number_integer() || quest.get<int64_t>() <= 0 ||
      quest.get<uint64_t>() > 0xffffffff || !stage.is_number_integer() ||
      stage.get<int64_t>() < 0 || stage.get<uint64_t>() > 65535)
    throw std::invalid_argument("Invalid quest stage request");
  if (parsed.contains("expectedRevision") &&
      parsed.at("expectedRevision") !=
        QuestJournal::Snapshot(actor).at("revision"))
    throw std::runtime_error("Stale quest revision");
  QuestScriptRuntime::ExecuteStage(actor, quest.get<uint32_t>(),
                                   stage.get<uint16_t>());
}
