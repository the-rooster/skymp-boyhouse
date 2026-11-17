#include "QuestJobState.h"
#include "FormDesc.h"
#include "QuestVmState.h"
#include <charconv>
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

void Bounded(const std::string& text, size_t limit)
{
  if (text.size() > limit || text.find('\0') != std::string::npos)
    throw std::runtime_error("Quest job string out of bounds");
}

QuestJobState::Stage ReadStage(const Json& value, bool child)
{
  if (child)
    Fields(value,
           { "node", "child", "quest", "contentHash", "stage", "cursor",
             "accepted", "log", "script", "function", "token", "awaitKey",
             "failure" });
  QuestJobState::Stage stage;
  stage.node = Number(value.at("node"), QuestJobState::MaxOperations, child);
  stage.child = Number(value.at("child"), QuestJobState::MaxOperations);
  stage.quest = Identity(value.at("quest"), false);
  stage.contentHash = Text(value.at("contentHash"), 64);
  if (stage.contentHash.size() != 64 ||
      stage.contentHash.find_first_not_of("0123456789abcdef") !=
        std::string::npos)
    throw std::runtime_error("Invalid quest job content hash");
  stage.stage = static_cast<uint16_t>(Number(value.at("stage"), 65535));
  stage.cursor = Text(value.at("cursor"), 7);
  stage.accepted = Boolean(value.at("accepted"));
  stage.log = static_cast<uint32_t>(Number(value.at("log"), 100000));
  stage.script = Text(value.at("script"), 1024, true);
  stage.function = Text(value.at("function"), 1024, true);
  stage.token = Text(value.at("token"), 256, true);
  stage.awaitKey = Text(value.at("awaitKey"), 128, true);
  stage.failure = Text(value.at("failure"), 1024, true);
  const bool pending = stage.child || stage.log || !stage.script.empty() ||
    !stage.function.empty() || !stage.token.empty() ||
    !stage.awaitKey.empty() || !stage.failure.empty();
  if (stage.cursor == "begin") {
    if (pending || stage.accepted)
      throw std::runtime_error("Quest stage begin cursor is not pristine");
  } else if (stage.cursor == "waiting") {
    if (!stage.accepted || stage.script.empty() || stage.function.empty() ||
        stage.token.empty() || stage.awaitKey.empty())
      throw std::runtime_error("Invalid waiting quest stage cursor");
  } else if (stage.cursor == "done") {
    if (pending)
      throw std::runtime_error("Completed quest stage retains pending work");
  } else
    throw std::runtime_error("Unsupported quest stage cursor");
  return stage;
}

uint64_t Operation(const QuestJobState::Job& job, const std::string& token)
{
  Bounded(token, 256);
  const auto prefix = job.Scope() + ":";
  if (!token.starts_with(prefix))
    throw std::runtime_error("Quest operation belongs to another job");
  uint64_t result = 0;
  const auto begin = token.data() + prefix.size();
  const auto end = token.data() + token.size();
  const auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc() || parsed.ptr != end || !result ||
      result >= job.nextOperation || token != prefix + std::to_string(result))
    throw std::runtime_error("Invalid quest operation identity");
  return result;
}

QuestJobState::Job ReadJob(const Json& saved, uint64_t version)
{
  auto value = saved;
  if (version == 1) {
    Fields(value,
           { "id",     "generation", "segment",  "command",      "actor",
             "owner",  "quest",      "instance", "contentHash",  "stage",
             "cursor", "accepted",   "log",      "instructions", "steps",
             "script", "function",   "token",    "awaitKey",     "failure",
             "events", "completions" });
    if (!value.at("completions").is_object() ||
        value.at("completions").size() > QuestJobState::MaxOperations)
      throw std::runtime_error("Invalid legacy quest completions");
    value["node"] = 0;
    value["child"] = 0;
    value["children"] = Json::array();
    value["nextOperation"] = value.at("completions").size() + 1 +
      (value.at("cursor") == "waiting" ? 1 : 0);
  } else
    Fields(
      value,
      { "id",      "generation",  "segment",  "command",      "actor",
        "owner",   "quest",       "instance", "contentHash",  "stage",
        "cursor",  "accepted",    "log",      "instructions", "steps",
        "script",  "function",    "token",    "awaitKey",     "failure",
        "events",  "completions", "node",     "child",        "nextOperation",
        "children" });
  QuestJobState::Job job;
  static_cast<QuestJobState::Stage&>(job) = ReadStage(value, false);
  if (job.node)
    throw std::runtime_error("Root quest stage has a child identity");
  job.id = Number(value.at("id"), MaxIdentity, true);
  job.generation = Number(value.at("generation"), MaxIdentity, true);
  job.segment =
    Number(value.at("segment"),
           (version == 1 ? 1 : 2) * QuestJobState::MaxOperations + 1);
  job.nextOperation =
    Number(value.at("nextOperation"), QuestJobState::MaxOperations + 1, true);
  job.command = Text(value.at("command"), 128);
  job.actor = Identity(value.at("actor"), true);
  job.owner = Identity(value.at("owner"), true);
  job.instance = Text(value.at("instance"), 300, true);
  job.instructions =
    static_cast<uint32_t>(Number(value.at("instructions"), 100000));
  job.steps =
    static_cast<uint32_t>(Number(value.at("steps"), QuestJobState::MaxSteps));
  const auto& children = value.at("children");
  if (!children.is_array() || children.size() >= QuestJobState::MaxDepth)
    throw std::runtime_error("Quest child stack depth exceeded");
  for (const auto& child : children)
    job.children.push_back(ReadStage(child, true));
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
  std::set<uint64_t> operations;
  for (const auto& [token, result] : completions.items()) {
    if (!operations.insert(Operation(job, token)).second)
      throw std::runtime_error("Duplicate quest completion");
    job.completions.emplace(token, Boolean(result));
  }
  if (version == 1 && job.cursor == "waiting" &&
      job.token !=
        job.Scope() + ":" + std::to_string(job.completions.size() + 1))
    throw std::runtime_error("Invalid legacy pending quest operation");
  std::vector<const QuestJobState::Stage*> stages{ &job };
  for (const auto& child : job.children)
    stages.push_back(&child);
  for (size_t index = 0; index < stages.size(); ++index) {
    const auto& stage = *stages[index];
    if (stage.cursor == "waiting") {
      if (!job.segment || !job.instructions ||
          !operations.insert(Operation(job, stage.token)).second)
        throw std::runtime_error(
          "Invalid or duplicate pending quest operation");
    }
    if (index + 1 < stages.size()) {
      const auto& child = *stages[index + 1];
      if (stage.cursor != "waiting" || stage.awaitKey != "quest-stage" ||
          stage.child != child.node || child.node <= stage.node ||
          Operation(job, stage.token) != child.node)
        throw std::runtime_error(
          "Quest stage stack has inconsistent parent links");
    } else if (stage.child)
      throw std::runtime_error("Quest stage has a missing child");
  }
  // Each allocated number is either an outstanding operation or one retained
  // completion, including parent waits whose children are still on the stack.
  if (operations.size() != job.nextOperation - 1)
    throw std::runtime_error("Quest operation history has missing identities");
  if (job.cursor == "begin" &&
      (job.segment || job.instructions || job.steps ||
       job.nextOperation != 1 || !job.events.empty() ||
       !job.completions.empty() || !job.children.empty()))
    throw std::runtime_error("Quest job begin cursor is not pristine");
  if (job.cursor == "done" && (!job.children.empty() || !job.events.empty()))
    throw std::runtime_error("Completed quest job retains pending work");
  return job;
}

Json SaveStage(const QuestJobState::Stage& stage)
{
  Bounded(stage.quest, 300);
  Bounded(stage.contentHash, 64);
  Bounded(stage.cursor, 7);
  for (const auto* text : { &stage.script, &stage.function, &stage.failure })
    Bounded(*text, 1024);
  Bounded(stage.token, 256);
  Bounded(stage.awaitKey, 128);
  return {
    { "node", stage.node },         { "child", stage.child },
    { "quest", stage.quest },       { "contentHash", stage.contentHash },
    { "stage", stage.stage },       { "cursor", stage.cursor },
    { "accepted", stage.accepted }, { "log", stage.log },
    { "script", stage.script },     { "function", stage.function },
    { "token", stage.token },       { "awaitKey", stage.awaitKey },
    { "failure", stage.failure }
  };
}

Json SaveJob(const QuestJobState::Job& job)
{
  Bounded(job.command, 128);
  for (const auto* text : { &job.actor, &job.owner, &job.instance })
    Bounded(*text, 300);
  if (job.events.size() > QuestJobState::MaxEvents ||
      job.completions.size() > QuestJobState::MaxOperations ||
      job.children.size() >= QuestJobState::MaxDepth)
    throw std::runtime_error("Quest job collection limit exceeded");
  auto events = Json::array();
  for (const auto& event : job.events) {
    for (const auto* text : { &event.target, &event.item, &event.container })
      Bounded(*text, 300);
    events.push_back({ { "target", event.target },
                       { "item", event.item },
                       { "container", event.container },
                       { "count", event.count },
                       { "added", event.added } });
  }
  for (const auto& [token, result] : job.completions)
    Bounded(token, 256);
  auto children = Json::array();
  for (const auto& child : job.children)
    children.push_back(SaveStage(child));
  auto result = SaveStage(job);
  result.update({ { "id", job.id },
                  { "generation", job.generation },
                  { "segment", job.segment },
                  { "nextOperation", job.nextOperation },
                  { "command", job.command },
                  { "actor", job.actor },
                  { "owner", job.owner },
                  { "instance", job.instance },
                  { "instructions", job.instructions },
                  { "steps", job.steps },
                  { "events", std::move(events) },
                  { "completions", job.completions },
                  { "children", std::move(children) } });
  return result;
}
}

std::string QuestJobState::Job::Scope() const
{
  return "quest-job:" + std::to_string(generation) + ":" + std::to_string(id);
}

std::string QuestJobState::Job::Scope(const Stage& stage) const
{
  return stage.node ? Scope() + ":node:" + std::to_string(stage.node)
                    : Scope();
}

QuestJobState::Stage& QuestJobState::Job::Current()
{
  return children.empty() ? static_cast<Stage&>(*this) : children.back();
}

const QuestJobState::Stage& QuestJobState::Job::Current() const
{
  return children.empty() ? static_cast<const Stage&>(*this) : children.back();
}

std::string QuestJobState::Job::NextToken() const
{
  return Scope() + ":" + std::to_string(nextOperation);
}

QuestJobState QuestJobState::Read(const nlohmann::json& value)
{
  Fields(value,
         { "schemaVersion", "generation", "nextJob", "active", "completed" });
  const auto version = Number(value.at("schemaVersion"), 2, true);
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
    auto job = ReadJob(value, version);
    if (job.cursor != "done")
      throw std::runtime_error("Incomplete quest job in history");
    identity(job);
    result.completed.push_back(std::move(job));
  }
  if (!value.at("active").is_null()) {
    if (result.completed.size() == MaxCompleted)
      throw std::runtime_error("Quest job completion history is full");
    result.active = ReadJob(value.at("active"), version);
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
  Json result = { { "schemaVersion", 2 },
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
  std::vector<const Stage*> waiting;
  if (active) {
    if (active->cursor == "waiting")
      waiting.push_back(&*active);
    for (const auto& child : active->children)
      if (child.cursor == "waiting")
        waiting.push_back(&child);
  }
  if (continuations.size() != waiting.size())
    throw std::runtime_error("Missing or orphan quest job continuation");
  for (const auto* stage : waiting) {
    if (!continuations.contains(stage->token))
      throw std::runtime_error("Missing quest stage continuation");
    const auto& continuation = continuations.at(stage->token);
    const auto& callers = continuation.at("callers");
    const auto& outer = callers.empty() ? continuation : callers.back();
    const auto instructions =
      continuation.at("executedInstructions").get<uint64_t>();
    if (continuation.at("scope") != active->Scope(*stage) || !instructions ||
        instructions > active->instructions ||
        (stage == &active->Current() &&
         instructions != active->instructions) ||
        outer.at("kind") != "method" || outer.at("state") != "" ||
        !SameScriptName(outer.at("rootScript"), stage->script) ||
        !SameScriptName(outer.at("script"), stage->script) ||
        !SameScriptName(outer.at("function"), stage->function) ||
        outer.at("receiver").at("type") != VarValue::kType_Object ||
        outer.at("receiver").at("value") != stage->quest)
      throw std::runtime_error(
        "Quest stage continuation does not match its cursor");
  }
}
