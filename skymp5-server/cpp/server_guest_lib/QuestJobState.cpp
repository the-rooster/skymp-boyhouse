#include "QuestJobState.h"
#include "FormDesc.h"
#include "QuestVmState.h"
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
constexpr uint64_t MaxIdentity = 9007199254740991ULL;

void Fields(const Json& value, std::initializer_list<const char*> fields)
{
  if (!value.is_object() || value.size() != fields.size())
    throw std::runtime_error("Invalid quest job fields");
  for (const auto* field : fields)
    if (!value.contains(field))
      throw std::runtime_error("Missing quest job field");
}

uint64_t Number(const Json& value, uint64_t limit, bool positive = false)
{
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<int64_t>() < 0))
    throw std::runtime_error("Invalid quest job integer");
  const auto result = value.get<uint64_t>();
  if (result > limit || (positive && !result))
    throw std::runtime_error("Quest job integer out of bounds");
  return result;
}

std::string Text(const Json& value, size_t limit, bool empty = false)
{
  if (!value.is_string())
    throw std::runtime_error("Invalid quest job string");
  const auto& result = value.get_ref<const std::string&>();
  if ((!empty && result.empty()) || result.size() > limit ||
      result.find('\0') != std::string::npos)
    throw std::runtime_error("Quest job string out of bounds");
  return result;
}

bool Boolean(const Json& value)
{
  if (!value.is_boolean())
    throw std::runtime_error("Invalid quest job boolean");
  return value.get<bool>();
}

bool SameScriptName(const Json& value, const std::string& expected)
{
  const auto& actual = value.get_ref<const std::string&>();
  if (actual.size() != expected.size())
    return false;
  const auto lower = [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? char(ch + ('a' - 'A')) : ch;
  };
  for (size_t index = 0; index < actual.size(); ++index)
    if (lower(actual[index]) != lower(expected[index]))
      return false;
  return true;
}

std::string Identity(const Json& value, bool dynamic, bool empty = false)
{
  const auto text = Text(value, 300, empty);
  if (empty && text.empty())
    return text;
  const auto form = FormDesc::FromString(text);
  if (form.ToString() != text || form.shortFormId > 0xffffff ||
      form.file.empty() != dynamic || (!dynamic && !form.shortFormId) ||
      form.file.find_first_of(":/\\") != std::string::npos)
    throw std::runtime_error("Invalid canonical quest job form identity");
  return text;
}

QuestJobState::Job ReadJob(const Json& value)
{
  Fields(value,
         { "id",     "generation", "segment",  "command",      "actor",
           "owner",  "quest",      "instance", "contentHash",  "stage",
           "cursor", "accepted",   "log",      "instructions", "steps",
           "script", "function",   "token",    "awaitKey",     "failure",
           "events", "completions" });
  QuestJobState::Job job;
  job.id = Number(value.at("id"), MaxIdentity, true);
  job.generation = Number(value.at("generation"), MaxIdentity, true);
  job.segment = Number(value.at("segment"), QuestJobState::MaxOperations + 1);
  job.command = Text(value.at("command"), 128);
  job.actor = Identity(value.at("actor"), true);
  job.owner = Identity(value.at("owner"), true);
  job.quest = Identity(value.at("quest"), false);
  job.instance = Text(value.at("instance"), 300, true);
  job.contentHash = Text(value.at("contentHash"), 64);
  if (job.contentHash.size() != 64 ||
      job.contentHash.find_first_not_of("0123456789abcdef") !=
        std::string::npos)
    throw std::runtime_error("Invalid quest job content hash");
  job.stage = static_cast<uint16_t>(Number(value.at("stage"), 65535));
  job.cursor = Text(value.at("cursor"), 7);
  job.accepted = Boolean(value.at("accepted"));
  job.log = static_cast<uint32_t>(Number(value.at("log"), 100000));
  job.instructions =
    static_cast<uint32_t>(Number(value.at("instructions"), 100000));
  job.steps =
    static_cast<uint32_t>(Number(value.at("steps"), QuestJobState::MaxSteps));
  job.script = Text(value.at("script"), 1024, true);
  job.function = Text(value.at("function"), 1024, true);
  job.token = Text(value.at("token"), 256, true);
  job.awaitKey = Text(value.at("awaitKey"), 128, true);
  job.failure = Text(value.at("failure"), 1024, true);
  const auto& events = value.at("events");
  if (!events.is_array() || events.size() > QuestJobState::MaxEvents)
    throw std::runtime_error("Quest job event limit exceeded");
  for (const auto& event : events) {
    Fields(event, { "target", "item", "container", "count", "added" });
    job.events.push_back(
      { Identity(event.at("target"), false), Identity(event.at("item"), false),
        Identity(event.at("container"), false, true),
        static_cast<uint32_t>(Number(event.at("count"), INT32_MAX, true)),
        Boolean(event.at("added")) });
  }
  const auto& completions = value.at("completions");
  if (!completions.is_object() ||
      completions.size() > QuestJobState::MaxOperations)
    throw std::runtime_error("Quest job operation limit exceeded");
  for (size_t index = 1; index <= completions.size(); ++index) {
    const auto token = job.Scope() + ":" + std::to_string(index);
    if (!completions.contains(token))
      throw std::runtime_error("Invalid quest job completion sequence");
    job.completions.emplace(token, Boolean(completions.at(token)));
  }
  if (job.cursor == "begin") {
    if (job.segment || job.accepted || job.log || job.instructions ||
        job.steps || !job.script.empty() || !job.function.empty() ||
        !job.token.empty() || !job.awaitKey.empty() || !job.failure.empty() ||
        !job.events.empty() || !job.completions.empty())
      throw std::runtime_error("Quest job begin cursor is not pristine");
  } else if (job.cursor == "waiting") {
    if (!job.accepted || !job.segment || !job.instructions ||
        job.script.empty() || job.function.empty() || job.awaitKey.empty() ||
        job.token != job.NextToken() ||
        job.completions.size() >= QuestJobState::MaxOperations)
      throw std::runtime_error("Invalid waiting quest job cursor");
  } else if (job.cursor == "done") {
    if (job.log || !job.script.empty() || !job.function.empty() ||
        !job.token.empty() || !job.awaitKey.empty() || !job.failure.empty() ||
        !job.events.empty())
      throw std::runtime_error("Completed quest job retains pending work");
  } else
    throw std::runtime_error("Unsupported quest job cursor");
  return job;
}

Json SaveJob(const QuestJobState::Job& job)
{
  const auto bounded = [](const std::string& text, size_t limit) {
    if (text.size() > limit || text.find('\0') != std::string::npos)
      throw std::runtime_error("Quest job string out of bounds");
  };
  bounded(job.command, 128);
  for (const auto* text :
       { &job.actor, &job.owner, &job.quest, &job.instance })
    bounded(*text, 300);
  bounded(job.contentHash, 64);
  bounded(job.cursor, 7);
  for (const auto* text : { &job.script, &job.function, &job.failure })
    bounded(*text, 1024);
  bounded(job.token, 256);
  bounded(job.awaitKey, 128);
  auto events = Json::array();
  if (job.events.size() > QuestJobState::MaxEvents ||
      job.completions.size() > QuestJobState::MaxOperations)
    throw std::runtime_error("Quest job collection limit exceeded");
  for (const auto& event : job.events) {
    for (const auto* text : { &event.target, &event.item, &event.container })
      bounded(*text, 300);
    events.push_back({ { "target", event.target },
                       { "item", event.item },
                       { "container", event.container },
                       { "count", event.count },
                       { "added", event.added } });
  }
  for (const auto& [token, result] : job.completions)
    bounded(token, 256);
  return { { "id", job.id },
           { "generation", job.generation },
           { "segment", job.segment },
           { "command", job.command },
           { "actor", job.actor },
           { "owner", job.owner },
           { "quest", job.quest },
           { "instance", job.instance },
           { "contentHash", job.contentHash },
           { "stage", job.stage },
           { "cursor", job.cursor },
           { "accepted", job.accepted },
           { "log", job.log },
           { "instructions", job.instructions },
           { "steps", job.steps },
           { "script", job.script },
           { "function", job.function },
           { "token", job.token },
           { "awaitKey", job.awaitKey },
           { "failure", job.failure },
           { "events", std::move(events) },
           { "completions", job.completions } };
}
}

std::string QuestJobState::Job::Scope() const
{
  return "quest-job:" + std::to_string(generation) + ":" + std::to_string(id);
}

std::string QuestJobState::Job::NextToken() const
{
  return Scope() + ":" + std::to_string(completions.size() + 1);
}

QuestJobState QuestJobState::Read(const nlohmann::json& value)
{
  Fields(value,
         { "schemaVersion", "generation", "nextJob", "active", "completed" });
  if (Number(value.at("schemaVersion"), 1, true) != 1)
    throw std::runtime_error("Unsupported quest job schema");
  QuestJobState result;
  result.generation = Number(value.at("generation"), MaxIdentity, true);
  result.nextJob = Number(value.at("nextJob"), MaxIdentity, true);
  const auto& completed = value.at("completed");
  if (!completed.is_array() || completed.size() > MaxCompleted)
    throw std::runtime_error("Quest job history limit exceeded");
  uint64_t previous = 0;
  std::set<std::string> commands;
  auto identity = [&](const Job& job) {
    if (job.generation != result.generation || job.id != previous + 1 ||
        job.id >= result.nextJob || !commands.insert(job.command).second)
      throw std::runtime_error("Inconsistent quest job identity sequence");
    previous = job.id;
  };
  for (const auto& value : completed) {
    auto job = ReadJob(value);
    if (job.cursor != "done")
      throw std::runtime_error("Incomplete quest job in history");
    identity(job);
    result.completed.push_back(std::move(job));
  }
  if (!value.at("active").is_null()) {
    if (result.completed.size() == MaxCompleted)
      throw std::runtime_error("Quest job completion history is full");
    result.active = ReadJob(value.at("active"));
    if (result.active->cursor == "done")
      throw std::runtime_error("Completed quest job is still active");
    identity(*result.active);
  }
  if (result.nextJob != previous + 1)
    throw std::runtime_error("Quest job history has missing identities");
  return result;
}

nlohmann::json QuestJobState::Save() const
{
  if (completed.size() > MaxCompleted)
    throw std::runtime_error("Quest job history limit exceeded");
  auto history = Json::array();
  for (const auto& job : completed)
    history.push_back(SaveJob(job));
  Json result = { { "schemaVersion", 1 },
                  { "generation", generation },
                  { "nextJob", nextJob },
                  { "active", active ? SaveJob(*active) : Json(nullptr) },
                  { "completed", std::move(history) } };
  Read(
    result); // Typed candidates must satisfy the same invariants as storage.
  return result;
}

void QuestJobState::ValidateHeap(const nlohmann::json& heap) const
{
  Save();
  QuestVmState codec(
    heap,
    [](IGameObject*) -> Json {
      throw std::runtime_error("Unexpected job object encoding");
    },
    [](const Json&) -> VarValue {
      throw std::runtime_error("Unexpected job object resolution");
    });
  const auto normalized = codec.Save();
  const auto& continuations = normalized.at("continuations");
  // Before the runner has ever allocated a job, preserve schema-1 journals'
  // standalone VM fixtures. Once used, the runner owns every saved stack.
  if (generation == 1 && nextJob == 1 && !active && completed.empty())
    return;
  if (!active || active->cursor != "waiting") {
    if (!continuations.empty())
      throw std::runtime_error("Orphan quest job continuation");
    return;
  }
  const auto& job = *active;
  if (continuations.size() != 1 || !continuations.contains(job.token))
    throw std::runtime_error("Missing or orphan quest job continuation");
  const auto& continuation = continuations.at(job.token);
  const auto& callers = continuation.at("callers");
  const auto& outer = callers.empty() ? continuation : callers.back();
  if (continuation.at("scope") != job.Scope() ||
      continuation.at("executedInstructions") != job.instructions ||
      outer.at("kind") != "method" || outer.at("state") != "" ||
      !SameScriptName(outer.at("rootScript"), job.script) ||
      !SameScriptName(outer.at("script"), job.script) ||
      !SameScriptName(outer.at("function"), job.function) ||
      outer.at("receiver").at("type") != VarValue::kType_Object ||
      outer.at("receiver").at("value") != job.quest)
    throw std::runtime_error(
      "Quest job continuation does not match its cursor");
}
