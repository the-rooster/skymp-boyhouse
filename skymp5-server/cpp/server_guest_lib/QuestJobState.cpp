#include "QuestJobState.h"
#include "FormDesc.h"
#include "QuestVmState.h"
#include <algorithm>
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

Json Recipient(const Json& value)
{
  if (value.is_string())
    return Identity(value, false);
  Fields(value, { "kind", "quest", "id" });
  if (Text(value.at("kind"), 5) != "alias")
    throw std::runtime_error("Invalid quest event recipient");
  return { { "kind", "alias" },
           { "quest", Identity(value.at("quest"), false) },
           { "id", Number(value.at("id"), INT32_MAX) } };
}

QuestJobState::HandlerIdentity ReadHandlerIdentity(const Json& value)
{
  Fields(value,
         { "receiver", "rootScript", "implementation", "state", "function",
           "sequence" });
  return { Recipient(value.at("receiver")),
           Text(value.at("rootScript"), 1024),
           Text(value.at("implementation"), 1024),
           Text(value.at("state"), 1024, true),
           Text(value.at("function"), 1024),
           Number(value.at("sequence"), QuestJobState::MaxHandlers, true) };
}

Json SaveHandlerIdentity(const QuestJobState::HandlerIdentity& identity)
{
  for (const auto* text : { &identity.rootScript, &identity.implementation,
                            &identity.state, &identity.function })
    Bounded(*text, 1024);
  return { { "receiver", Recipient(identity.receiver) },
           { "rootScript", identity.rootScript },
           { "implementation", identity.implementation },
           { "state", identity.state },
           { "function", identity.function },
           { "sequence", identity.sequence } };
}

QuestJobState::Dispatch ReadDispatch(const Json& value)
{
  Fields(value,
         { "recipients", "recipient", "attachment", "captured", "roots",
           "templates", "reference" });
  QuestJobState::Dispatch result;
  const auto& recipients = value.at("recipients");
  const auto& roots = value.at("roots");
  const auto& templates = value.at("templates");
  if (!recipients.is_array() || recipients.empty() ||
      recipients.size() > QuestJobState::MaxRecipients || !roots.is_array() ||
      roots.size() > QuestJobState::MaxAttachments || !templates.is_array() ||
      templates.size() > 128)
    throw std::runtime_error("Quest event roster limit exceeded");
  std::set<std::string> seen;
  for (const auto& recipient : recipients) {
    const auto identity = Recipient(recipient);
    if (!seen.insert(identity.dump()).second)
      throw std::runtime_error("Duplicate quest event recipient");
    result.recipients.push_back(identity);
  }
  for (const auto& root : roots) {
    const auto name = Text(root, 1024);
    for (const auto& existing : result.roots)
      if (SameScriptName(root, existing))
        throw std::runtime_error("Duplicate quest event attachment");
    result.roots.push_back(name);
  }
  seen.clear();
  for (const auto& item : templates) {
    const auto name = Identity(item, false);
    if (!seen.insert(name).second)
      throw std::runtime_error("Duplicate quest event template");
    result.templates.push_back(name);
  }
  result.recipient = static_cast<uint32_t>(
    Number(value.at("recipient"), result.recipients.size()));
  result.attachment =
    static_cast<uint32_t>(Number(value.at("attachment"), result.roots.size()));
  result.captured = Boolean(value.at("captured"));
  result.reference = Text(value.at("reference"), 300, true);
  if (!result.captured) {
    if (result.attachment || !result.roots.empty() ||
        !result.templates.empty() || !result.reference.empty())
      throw std::runtime_error(
        "Uncaptured quest event retains attachment state");
  } else {
    if (result.recipient == result.recipients.size())
      throw std::runtime_error("Captured quest event has no recipient");
    if (result.recipients[result.recipient].is_string())
      Identity(value.at("reference"), true);
    else if (!result.templates.empty() || !result.reference.empty())
      throw std::runtime_error(
        "Alias event retains reference attachment state");
  }
  return result;
}

Json SaveDispatch(const QuestJobState::Dispatch& dispatch)
{
  if (dispatch.recipients.size() > QuestJobState::MaxRecipients ||
      dispatch.roots.size() > QuestJobState::MaxAttachments ||
      dispatch.templates.size() > 128)
    throw std::runtime_error("Quest event roster limit exceeded");
  for (const auto& recipient : dispatch.recipients)
    Recipient(recipient);
  for (const auto& root : dispatch.roots)
    Bounded(root, 1024);
  for (const auto& item : dispatch.templates)
    Bounded(item, 300);
  Bounded(dispatch.reference, 300);
  return {
    { "recipients", dispatch.recipients }, { "recipient", dispatch.recipient },
    { "attachment", dispatch.attachment }, { "captured", dispatch.captured },
    { "roots", dispatch.roots },           { "templates", dispatch.templates },
    { "reference", dispatch.reference }
  };
}

std::vector<std::string> ReadTemplates(const Json& values)
{
  if (!values.is_array() || values.size() > 128)
    throw std::runtime_error("Quest reference template limit exceeded");
  std::vector<std::string> result;
  std::set<std::string> seen;
  for (const auto& value : values) {
    const auto identity = Identity(value, false);
    if (!seen.insert(identity).second)
      throw std::runtime_error("Duplicate quest reference template");
    result.push_back(identity);
  }
  return result;
}

QuestJobState::Interaction ReadInteraction(const Json& value)
{
  Fields(value,
         { "name", "target", "source", "targetReference", "sourceReference",
           "targetTemplates", "sourceTemplates" });
  QuestJobState::Interaction interaction;
  interaction.name = Text(value.at("name"), 32);
  if (interaction.name != "OnActivate" &&
      interaction.name != "OnTriggerEnter" &&
      interaction.name != "OnTriggerLeave" &&
      interaction.name != "OnTrigger" &&
      interaction.name != "SkympOnActivateClose")
    throw std::runtime_error("Unsupported quest interaction event");
  interaction.target = Identity(value.at("target"), false);
  interaction.source = Identity(value.at("source"), false);
  interaction.targetReference = Identity(value.at("targetReference"), true);
  interaction.sourceReference = Identity(value.at("sourceReference"), true);
  interaction.targetTemplates = ReadTemplates(value.at("targetTemplates"));
  interaction.sourceTemplates = ReadTemplates(value.at("sourceTemplates"));
  return interaction;
}

Json SaveInteraction(const QuestJobState::Interaction& interaction)
{
  Bounded(interaction.name, 32);
  for (const auto* text :
       { &interaction.target, &interaction.source,
         &interaction.targetReference, &interaction.sourceReference })
    Bounded(*text, 300);
  for (const auto* templates :
       { &interaction.targetTemplates, &interaction.sourceTemplates }) {
    if (templates->size() > 128)
      throw std::runtime_error("Quest reference template limit exceeded");
    for (const auto& name : *templates)
      Bounded(name, 300);
  }
  return { { "name", interaction.name },
           { "target", interaction.target },
           { "source", interaction.source },
           { "targetReference", interaction.targetReference },
           { "sourceReference", interaction.sourceReference },
           { "targetTemplates", interaction.targetTemplates },
           { "sourceTemplates", interaction.sourceTemplates } };
}

QuestJobState::ReferenceLoad ReadReferenceLoad(const Json& value)
{
  Fields(value, { "target", "reference", "templates" });
  QuestJobState::ReferenceLoad load{ Identity(value.at("target"), false),
                                     Identity(value.at("reference"), true),
                                     ReadTemplates(value.at("templates")) };
  if (load.target == "14:Skyrim.esm")
    throw std::runtime_error("PlayerRef cannot be a reference-load target");
  return load;
}

Json SaveReferenceLoad(const QuestJobState::ReferenceLoad& load)
{
  Bounded(load.target, 300);
  Bounded(load.reference, 300);
  if (load.templates.size() > 128)
    throw std::runtime_error("Reference-load template limit exceeded");
  for (const auto& name : load.templates)
    Bounded(name, 300);
  return { { "target", load.target },
           { "reference", load.reference },
           { "templates", load.templates } };
}

QuestJobState::LoadPhase ReadLoadPhase(const Json& value)
{
  const auto phase = Text(value, 8);
  if (phase == "init")
    return QuestJobState::LoadPhase::Init;
  if (phase == "cellLoad")
    return QuestJobState::LoadPhase::CellLoad;
  if (phase == "load")
    return QuestJobState::LoadPhase::Load;
  if (phase == "done")
    return QuestJobState::LoadPhase::Done;
  throw std::runtime_error("Unsupported reference-load phase");
}

const char* SaveLoadPhase(QuestJobState::LoadPhase phase)
{
  switch (phase) {
    case QuestJobState::LoadPhase::Init:
      return "init";
    case QuestJobState::LoadPhase::CellLoad:
      return "cellLoad";
    case QuestJobState::LoadPhase::Load:
      return "load";
    case QuestJobState::LoadPhase::Done:
      return "done";
  }
  throw std::runtime_error("Unsupported typed reference-load phase");
}

bool InertHostRoot(const QuestJobState::Stage& stage)
{
  QuestJobState::Stage inert;
  inert.cursor = "done";
  return stage == inert;
}

QuestJobState::Stage ReadStage(const Json& value, bool child, uint64_t version,
                               bool handler = false, bool emptyAnchor = false)
{
  if (child || handler) {
    auto fields = value;
    if (version >= 4)
      fields.erase("handlerIdentity");
    if (version < 3)
      Fields(fields,
             { "node", "child", "quest", "contentHash", "stage", "cursor",
               "accepted", "log", "script", "function", "token", "awaitKey",
               "failure" });
    else
      Fields(fields,
             { "node", "child", "quest", "contentHash", "stage", "cursor",
               "accepted", "log", "script", "function", "token", "awaitKey",
               "failure", "awaitKind", "timer" });
  }
  QuestJobState::Stage stage;
  if (version >= 4 && !value.at("handlerIdentity").is_null())
    stage.handlerIdentity = ReadHandlerIdentity(value.at("handlerIdentity"));
  if (handler != stage.handlerIdentity.has_value())
    throw std::runtime_error("Unexpected quest event handler identity");
  stage.node = Number(value.at("node"), QuestJobState::MaxOperations, child);
  if (handler && stage.node)
    throw std::runtime_error(
      "Quest event handler cannot use a native child node");
  stage.child = Number(value.at("child"), QuestJobState::MaxOperations);
  stage.quest = Identity(value.at("quest"), false, emptyAnchor);
  stage.contentHash = Text(value.at("contentHash"), 64, emptyAnchor);
  stage.stage = static_cast<uint16_t>(Number(value.at("stage"), 65535));
  if (emptyAnchor) {
    if (!stage.quest.empty() || !stage.contentHash.empty() || stage.stage)
      throw std::runtime_error(
        "Interaction handler retains a quest stage anchor");
  } else if (stage.contentHash.size() != 64 ||
             stage.contentHash.find_first_not_of("0123456789abcdef") !=
               std::string::npos)
    throw std::runtime_error("Invalid quest job content hash");
  stage.cursor = Text(value.at("cursor"), 7);
  stage.accepted = Boolean(value.at("accepted"));
  stage.log = static_cast<uint32_t>(Number(value.at("log"), 100000));
  stage.script = Text(value.at("script"), 1024, true);
  stage.function = Text(value.at("function"), 1024, true);
  stage.token = Text(value.at("token"), 256, true);
  stage.awaitKey = Text(value.at("awaitKey"), 128, true);
  stage.failure = Text(value.at("failure"), 1024, true);
  stage.awaitKind = version < 3
    ? (stage.cursor == "waiting" ? (stage.child ? "stage" : "bool") : "")
    : Text(value.at("awaitKind"), 5, true);
  if (version >= 3 && !value.at("timer").is_null()) {
    const auto& timer = value.at("timer");
    Fields(timer, { "policy", "issuedUnixMs", "dueUnixMs", "durationMs" });
    if (Text(timer.at("policy"), 15) != "utc-deadline-v1")
      throw std::runtime_error("Unsupported quest timer policy");
    stage.timer =
      QuestJobState::Timer{ Number(timer.at("issuedUnixMs"), MaxIdentity),
                            Number(timer.at("dueUnixMs"), MaxIdentity),
                            Number(timer.at("durationMs"), 86400000) };
    if (stage.timer->durationMs > MaxIdentity - stage.timer->issuedUnixMs ||
        stage.timer->dueUnixMs !=
          stage.timer->issuedUnixMs + stage.timer->durationMs)
      throw std::runtime_error("Invalid quest timer deadline");
  }
  const bool pending = stage.child || stage.log || !stage.script.empty() ||
    !stage.function.empty() || !stage.token.empty() ||
    !stage.awaitKey.empty() || !stage.awaitKind.empty() || stage.timer ||
    !stage.failure.empty();
  if (stage.cursor == "begin") {
    if (pending || stage.accepted)
      throw std::runtime_error("Quest stage begin cursor is not pristine");
  } else if (stage.cursor == "waiting") {
    if (!stage.accepted || stage.script.empty() || stage.function.empty() ||
        stage.token.empty() || stage.awaitKey.empty() ||
        (stage.awaitKind != "bool" && stage.awaitKind != "stage" &&
         stage.awaitKind != "timer") ||
        ((stage.awaitKind == "stage") != (stage.child != 0)) ||
        ((stage.awaitKind == "timer") != stage.timer.has_value()))
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
  } else if (version == 2)
    Fields(
      value,
      { "id",      "generation",  "segment",  "command",      "actor",
        "owner",   "quest",       "instance", "contentHash",  "stage",
        "cursor",  "accepted",    "log",      "instructions", "steps",
        "script",  "function",    "token",    "awaitKey",     "failure",
        "events",  "completions", "node",     "child",        "nextOperation",
        "children" });
  else {
    auto fields = value;
    if (version >= 8)
      for (const auto* field : { "referenceLoad", "loadPhase" })
        fields.erase(field);
    if (version >= 7)
      for (const auto* field : { "kind", "interaction", "interactionDone" })
        fields.erase(field);
    if (version >= 6)
      fields.erase("environmentHash");
    if (version >= 5)
      fields.erase("sequenced");
    if (version >= 4)
      for (const auto* field : { "handlerIdentity", "eventHead", "nextHandler",
                                 "dispatch", "handler" })
        fields.erase(field);
    Fields(fields, { "id",
                     "generation",
                     "segment",
                     "command",
                     "actor",
                     "owner",
                     "quest",
                     "instance",
                     "contentHash",
                     "stage",
                     "cursor",
                     "accepted",
                     "log",
                     "instructions",
                     "steps",
                     "script",
                     "function",
                     "token",
                     "awaitKey",
                     "failure",
                     "events",
                     "completions",
                     "node",
                     "child",
                     "nextOperation",
                     "children",
                     "automatic",
                     "awaitKind",
                     "timer",
                     "contentMode" });
  }
  QuestJobState::Job job;
  if (version >= 7) {
    const auto kind = Text(value.at("kind"), 13);
    if (kind == "stage")
      job.kind = QuestJobState::Kind::Stage;
    else if (kind == "interaction")
      job.kind = QuestJobState::Kind::Interaction;
    else if (version >= 8 && kind == "referenceLoad")
      job.kind = QuestJobState::Kind::ReferenceLoad;
    else
      throw std::runtime_error("Unsupported quest job kind");
    job.interactionDone = Boolean(value.at("interactionDone"));
    if (!value.at("interaction").is_null())
      job.interaction = ReadInteraction(value.at("interaction"));
  }
  if (version >= 8) {
    job.loadPhase = ReadLoadPhase(value.at("loadPhase"));
    if (!value.at("referenceLoad").is_null())
      job.referenceLoad = ReadReferenceLoad(value.at("referenceLoad"));
  }
  const bool interaction = job.kind == QuestJobState::Kind::Interaction;
  const bool referenceLoad = job.kind == QuestJobState::Kind::ReferenceLoad;
  const bool hostRoot = interaction || referenceLoad;
  if (referenceLoad != job.referenceLoad.has_value() ||
      (!referenceLoad && job.loadPhase != QuestJobState::LoadPhase::Done))
    throw std::runtime_error(
      "Quest job reference-load payload does not match its kind");
  if (interaction != job.interaction.has_value() ||
      (!interaction && !job.interactionDone))
    throw std::runtime_error(
      "Quest job interaction payload does not match its kind");
  static_cast<QuestJobState::Stage&>(job) =
    ReadStage(value, false, version, false, hostRoot);
  if (hostRoot && !InertHostRoot(job))
    throw std::runtime_error(
      "Host event root contains executable quest stage state");
  job.automatic = version >= 3 ? Boolean(value.at("automatic")) : false;
  job.sequenced = version >= 5 ? Boolean(value.at("sequenced")) : false;
  job.contentMode = version >= 3 ? Text(value.at("contentMode"), 7) : "legacy";
  job.environmentHash =
    version >= 6 ? Text(value.at("environmentHash"), 64, true) : "";
  if (!job.environmentHash.empty() &&
      (job.environmentHash.size() != 64 ||
       job.environmentHash.find_first_not_of("0123456789abcdef") !=
         std::string::npos))
    throw std::runtime_error("Invalid quest job environment hash");
  if ((job.contentMode != "owned" && job.contentMode != "fixture" &&
       job.contentMode != "legacy") ||
      (job.automatic && job.contentMode == "legacy") ||
      (hostRoot &&
       (job.environmentHash.empty() || job.contentMode == "legacy")))
    throw std::runtime_error("Invalid quest job content provenance");
  if (job.node)
    throw std::runtime_error("Root quest stage has a child identity");
  job.id = Number(value.at("id"), MaxIdentity, true);
  job.generation = Number(value.at("generation"), MaxIdentity, true);
  job.segment = Number(
    value.at("segment"),
    version >= 4 ? QuestJobState::MaxSegments
                 : (version == 1 ? 1 : 2) * QuestJobState::MaxOperations + 1);
  job.nextOperation =
    Number(value.at("nextOperation"), QuestJobState::MaxOperations + 1, true);
  job.command = Text(value.at("command"), 128);
  if (job.sequenced &&
      job.command !=
        "quest-request:" + std::to_string(job.generation) + ":" +
          std::to_string(job.id))
    throw std::runtime_error("Invalid canonical sequenced quest command");
  job.actor = Identity(value.at("actor"), true);
  job.owner = Identity(value.at("owner"), true);
  if (referenceLoad && job.actor != job.owner)
    throw std::runtime_error(
      "Reference-load root must execute as its campaign owner");
  job.instance = Text(value.at("instance"), 300, true);
  job.instructions =
    static_cast<uint32_t>(Number(value.at("instructions"), 100000));
  job.steps =
    static_cast<uint32_t>(Number(value.at("steps"), QuestJobState::MaxSteps));
  const auto& children = value.at("children");
  if (!children.is_array() || children.size() >= QuestJobState::MaxDepth)
    throw std::runtime_error("Quest child stack depth exceeded");
  for (const auto& child : children)
    job.children.push_back(ReadStage(child, true, version));
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
  if (version >= 4) {
    job.eventHead =
      static_cast<uint32_t>(Number(value.at("eventHead"), job.events.size()));
    job.nextHandler =
      Number(value.at("nextHandler"), QuestJobState::MaxHandlers + 1, true);
    if (!value.at("dispatch").is_null())
      job.dispatch = ReadDispatch(value.at("dispatch"));
    if (!value.at("handler").is_null())
      job.handler =
        ReadStage(value.at("handler"), false, version, true, hostRoot);
  }
  if (job.cursor != "done") {
    if (job.dispatch || job.handler || job.eventHead || job.nextHandler != 1)
      throw std::runtime_error(
        "Quest event dispatch preceded root completion");
  } else if (!job.handler && !job.children.empty())
    throw std::runtime_error("Completed quest root retains native children");
  if (version < 4 && job.cursor == "done" && !job.events.empty())
    throw std::runtime_error("Legacy completed quest retains events");
  const auto* primaryName = job.PrimaryEventName();
  const bool primary = primaryName != nullptr;
  if (primary && job.eventHead)
    throw std::runtime_error(
      "Inventory FIFO advanced before primary event completion");
  if (job.dispatch) {
    const auto& target = primary
      ? (referenceLoad ? job.referenceLoad->target : job.interaction->target)
      : (job.eventHead < job.events.size() ? job.events[job.eventHead].target
                                           : std::string{});
    if ((!primary && job.eventHead == job.events.size()) ||
        job.dispatch->recipients.front() != target)
      throw std::runtime_error(
        "Quest event dispatch has no matching target payload");
    if (referenceLoad && job.loadPhase == QuestJobState::LoadPhase::Init &&
        job.dispatch->recipients.size() != 1)
      throw std::runtime_error(
        "Reference initialization cannot fan out to aliases");
    if (primary && job.dispatch->captured && !job.dispatch->recipient) {
      const auto& reference = referenceLoad ? job.referenceLoad->reference
                                            : job.interaction->targetReference;
      const auto& templates = referenceLoad ? job.referenceLoad->templates
                                            : job.interaction->targetTemplates;
      if (job.dispatch->reference != reference ||
          job.dispatch->templates != templates)
        throw std::runtime_error(
          "Primary event dispatch changed its captured target binding");
    }
    for (size_t index = 1; index < job.dispatch->recipients.size(); ++index)
      if (!job.dispatch->recipients[index].is_object())
        throw std::runtime_error(
          "Quest event has a non-alias secondary recipient");
  }
  if (job.handler) {
    const auto& handler = *job.handler;
    const auto& identity = *handler.handlerIdentity;
    const std::string function = primary
      ? primaryName
      : (job.eventHead < job.events.size() && job.events[job.eventHead].added
           ? "OnItemAdded"
           : "OnItemRemoved");
    if (!job.dispatch || !job.dispatch->captured ||
        job.dispatch->attachment == job.dispatch->roots.size() ||
        handler.quest != job.quest || handler.contentHash != job.contentHash ||
        handler.stage != job.stage ||
        identity.sequence != job.nextHandler - 1 ||
        identity.receiver !=
          job.dispatch->recipients[job.dispatch->recipient] ||
        !SameScriptName(job.dispatch->roots[job.dispatch->attachment],
                        identity.rootScript) ||
        !SameScriptName(identity.function, function) ||
        (handler.cursor == "waiting" &&
         (!SameScriptName(handler.script, identity.implementation) ||
          !SameScriptName(handler.function, identity.function))))
      throw std::runtime_error(
        "Quest event handler does not match its roster");
  }
  const auto& completions = value.at("completions");
  if (!completions.is_object() ||
      completions.size() > QuestJobState::MaxOperations)
    throw std::runtime_error("Quest job operation limit exceeded");
  std::set<uint64_t> operations;
  for (const auto& [token, result] : completions.items()) {
    if (!operations.insert(Operation(job, token)).second)
      throw std::runtime_error("Duplicate quest completion");
    QuestJobState::Completion completion;
    if (version < 3)
      completion = Boolean(result);
    else {
      if (!result.is_object() || !result.contains("kind"))
        throw std::runtime_error("Invalid quest completion fields");
      const auto kind = Text(result.at("kind"), 4);
      if (kind == "bool") {
        Fields(result, { "kind", "value" });
        completion = Boolean(result.at("value"));
      } else if (kind == "none") {
        Fields(result, { "kind" });
        completion = QuestJobState::Completion::None();
      } else
        throw std::runtime_error("Unsupported quest completion kind");
    }
    job.completions.emplace(token, completion);
  }
  if (version == 1 && job.cursor == "waiting" &&
      job.token !=
        job.Scope() + ":" + std::to_string(job.completions.size() + 1))
    throw std::runtime_error("Invalid legacy pending quest operation");
  std::vector<const QuestJobState::Stage*> stages{ job.handler ? &*job.handler
                                                               : &job };
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
      if (stage.cursor != "waiting" || stage.awaitKind != "stage" ||
          stage.awaitKey != "quest-stage" || stage.child != child.node ||
          child.node <= stage.node ||
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
  if ((job.cursor == "begin" || (hostRoot && !job.segment)) &&
      !job.IsPristine())
    throw std::runtime_error("Quest job initial cursor is not pristine");
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
  Bounded(stage.awaitKind, 5);
  Json timer = nullptr;
  if (stage.timer)
    timer = { { "policy", "utc-deadline-v1" },
              { "issuedUnixMs", stage.timer->issuedUnixMs },
              { "dueUnixMs", stage.timer->dueUnixMs },
              { "durationMs", stage.timer->durationMs } };
  return { { "node", stage.node },
           { "child", stage.child },
           { "quest", stage.quest },
           { "contentHash", stage.contentHash },
           { "stage", stage.stage },
           { "cursor", stage.cursor },
           { "accepted", stage.accepted },
           { "log", stage.log },
           { "script", stage.script },
           { "function", stage.function },
           { "token", stage.token },
           { "awaitKey", stage.awaitKey },
           { "failure", stage.failure },
           { "awaitKind", stage.awaitKind },
           { "timer", std::move(timer) },
           { "handlerIdentity",
             stage.handlerIdentity
               ? SaveHandlerIdentity(*stage.handlerIdentity)
               : Json(nullptr) } };
}

Json SaveJob(const QuestJobState::Job& job)
{
  if (job.kind != QuestJobState::Kind::Stage &&
      job.kind != QuestJobState::Kind::Interaction &&
      job.kind != QuestJobState::Kind::ReferenceLoad)
    throw std::runtime_error("Unsupported typed quest job kind");
  Bounded(job.command, 128);
  Bounded(job.contentMode, 7);
  Bounded(job.environmentHash, 64);
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
  auto completions = Json::object();
  for (const auto& [token, result] : job.completions) {
    Bounded(token, 256);
    if (result.kind == QuestJobState::Completion::Kind::Boolean)
      completions[token] = { { "kind", "bool" }, { "value", result.value } };
    else if (result.kind == QuestJobState::Completion::Kind::None &&
             !result.value)
      completions[token] = { { "kind", "none" } };
    else
      throw std::runtime_error("Invalid typed quest completion");
  }
  auto children = Json::array();
  for (const auto& child : job.children)
    children.push_back(SaveStage(child));
  auto result = SaveStage(job);
  result.update(
    { { "kind",
        job.kind == QuestJobState::Kind::Stage
          ? "stage"
          : (job.kind == QuestJobState::Kind::Interaction ? "interaction"
                                                          : "referenceLoad") },
      { "interaction",
        job.interaction ? SaveInteraction(*job.interaction) : Json(nullptr) },
      { "interactionDone", job.interactionDone },
      { "referenceLoad",
        job.referenceLoad ? SaveReferenceLoad(*job.referenceLoad)
                          : Json(nullptr) },
      { "loadPhase", SaveLoadPhase(job.loadPhase) },
      { "id", job.id },
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
      { "completions", std::move(completions) },
      { "automatic", job.automatic },
      { "sequenced", job.sequenced },
      { "contentMode", job.contentMode },
      { "environmentHash", job.environmentHash },
      { "children", std::move(children) },
      { "eventHead", job.eventHead },
      { "nextHandler", job.nextHandler },
      { "dispatch",
        job.dispatch ? SaveDispatch(*job.dispatch) : Json(nullptr) },
      { "handler", job.handler ? SaveStage(*job.handler) : Json(nullptr) } });
  return result;
}
}

std::string QuestJobState::Job::Scope() const
{
  return "quest-job:" + std::to_string(generation) + ":" + std::to_string(id);
}

std::string QuestJobState::Job::Scope(const Stage& stage) const
{
  if (stage.handlerIdentity)
    return Scope() +
      ":handler:" + std::to_string(stage.handlerIdentity->sequence);
  return stage.node ? Scope() + ":node:" + std::to_string(stage.node)
                    : Scope();
}

QuestJobState::Stage& QuestJobState::Job::Current()
{
  return !children.empty() ? children.back()
    : handler              ? *handler
                           : static_cast<Stage&>(*this);
}

const QuestJobState::Stage& QuestJobState::Job::Current() const
{
  return !children.empty() ? children.back()
    : handler              ? *handler
                           : static_cast<const Stage&>(*this);
}

bool QuestJobState::Job::IsComplete() const
{
  return cursor == "done" && interactionDone && loadPhase == LoadPhase::Done &&
    children.empty() && !handler && !dispatch && eventHead == events.size();
}

bool QuestJobState::Job::IsPristine() const
{
  if (segment || instructions || steps || nextOperation != 1 ||
      nextHandler != 1 || eventHead || !children.empty() || handler ||
      dispatch || !events.empty() || !completions.empty())
    return false;
  if (kind == Kind::ReferenceLoad)
    return referenceLoad && loadPhase == LoadPhase::Init && !interaction &&
      interactionDone && InertHostRoot(*this);
  if (kind == Kind::Interaction)
    return interaction && !interactionDone && !referenceLoad &&
      loadPhase == LoadPhase::Done && InertHostRoot(*this);
  if (kind != Kind::Stage || interaction || !interactionDone ||
      referenceLoad || loadPhase != LoadPhase::Done)
    return false;
  Stage pristine;
  pristine.quest = quest;
  pristine.contentHash = contentHash;
  pristine.stage = stage;
  return static_cast<const Stage&>(*this) == pristine;
}

const char* QuestJobState::Job::PrimaryEventName() const
{
  if (kind == Kind::Interaction && interaction && !interactionDone)
    return interaction->name.c_str();
  if (kind == Kind::ReferenceLoad && referenceLoad) {
    switch (loadPhase) {
      case LoadPhase::Init:
        return "OnInit";
      case LoadPhase::CellLoad:
        return "OnCellLoad";
      case LoadPhase::Load:
        return "OnLoad";
      case LoadPhase::Done:
        return nullptr;
    }
  }
  return nullptr;
}

bool QuestJobState::Job::HasPrimaryEvent() const
{
  return PrimaryEventName() != nullptr;
}

std::string QuestJobState::Job::NextToken() const
{
  return Scope() + ":" + std::to_string(nextOperation);
}

QuestJobState QuestJobState::Read(const nlohmann::json& value)
{
  const auto version = Number(value.at("schemaVersion"), 8, true);
  if (version >= 6)
    Fields(value,
           { "schemaVersion", "generation", "nextJob", "retiredThrough",
             "retiredRanges", "live", "completed" });
  else if (version >= 5)
    Fields(value,
           { "schemaVersion", "generation", "nextJob", "retiredThrough",
             "active", "completed" });
  else
    Fields(
      value,
      { "schemaVersion", "generation", "nextJob", "active", "completed" });
  QuestJobState result;
  result.generation = Number(value.at("generation"), MaxIdentity, true);
  result.nextJob = Number(value.at("nextJob"), MaxIdentity, true);
  result.retiredThrough =
    version >= 5 ? Number(value.at("retiredThrough"), MaxIdentity) : 0;
  if (result.retiredThrough >= result.nextJob)
    throw std::runtime_error(
      "Quest retirement frontier exceeds assigned identities");
  const auto& completed = value.at("completed");
  if (!completed.is_array() ||
      completed.size() > MaxCompleted + MaxRecentCompleted)
    throw std::runtime_error("Quest job history limit exceeded");
  size_t legacyCount = 0, sequencedCount = 0;
  std::set<uint64_t> identities;
  std::set<std::pair<bool, std::string>> commands;
  std::vector<RetiredRange> coverage;
  const auto identity = [&](const Job& job, bool isLive) {
    if (job.generation != result.generation || job.id >= result.nextJob ||
        job.id == result.retiredThrough || !identities.insert(job.id).second ||
        !commands.emplace(job.sequenced, job.command).second ||
        ((isLive || job.sequenced) && job.id <= result.retiredThrough))
      throw std::runtime_error("Inconsistent quest job identity sequence");
    if (job.id > result.retiredThrough)
      coverage.push_back({ job.id, job.id });
    if (!job.sequenced && ++legacyCount > MaxCompleted)
      throw std::runtime_error("Legacy quest job history is full");
    if (job.sequenced && ++sequencedCount > MaxRecentCompleted)
      throw std::runtime_error(
        "Sequenced live and recent quest capacity exceeded");
  };
  uint64_t previous = 0;
  for (const auto& saved : completed) {
    auto job = ReadJob(saved, version);
    if (!job.IsComplete() || job.id <= previous)
      throw std::runtime_error("Incomplete or unordered quest job history");
    previous = job.id;
    identity(job, false);
    result.completed.push_back(std::move(job));
  }
  auto live = Json::array();
  if (version >= 6) {
    const auto& saved = value.at("live");
    if (!saved.is_array() || saved.size() > MaxLive)
      throw std::runtime_error("Live quest job limit exceeded");
    live = saved;
  } else if (!value.at("active").is_null())
    live.push_back(value.at("active"));
  previous = 0;
  size_t stageCount = 0;
  std::set<std::string> loadTargets;
  for (const auto& saved : live) {
    auto job = ReadJob(saved, version);
    if (job.IsComplete() || job.id <= previous ||
        (version < 6 && !result.completed.empty() &&
         job.id <= result.completed.back().id))
      throw std::runtime_error("Completed or unordered live quest job");
    previous = job.id;
    if (job.kind == Kind::Stage && ++stageCount > MaxStageLive)
      throw std::runtime_error("Live quest stage root limit exceeded");
    if (job.referenceLoad &&
        !loadTargets.insert(job.referenceLoad->target).second)
      throw std::runtime_error("Duplicate live reference-load target");
    identity(job, true);
    if (!result.live.empty()) {
      const auto& first = result.live.begin()->second;
      if (job.environmentHash.empty() ||
          job.environmentHash != first.environmentHash ||
          job.contentMode != first.contentMode || job.owner != first.owner)
        throw std::runtime_error(
          "Concurrent quest jobs have incompatible environments");
    }
    result.live.emplace(job.id, std::move(job));
  }
  if (version >= 6) {
    const auto& ranges = value.at("retiredRanges");
    if (!ranges.is_array() || ranges.size() > MaxRetiredRanges)
      throw std::runtime_error("Quest retirement range limit exceeded");
    uint64_t last = result.retiredThrough;
    for (const auto& range : ranges) {
      Fields(range, { "first", "last" });
      RetiredRange parsed{ Number(range.at("first"), MaxIdentity, true),
                           Number(range.at("last"), MaxIdentity, true) };
      if (parsed.first > parsed.last ||
          parsed.first <= result.retiredThrough ||
          parsed.last >= result.nextJob ||
          (!result.retiredRanges.empty() && parsed.first <= last + 1))
        throw std::runtime_error("Invalid canonical quest retirement range");
      last = parsed.last;
      result.retiredRanges.push_back(parsed);
      coverage.push_back(parsed);
    }
  }
  // Sweep retained identities and sparse intervals, never historical IDs.
  std::sort(coverage.begin(), coverage.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  auto expected = result.retiredThrough + 1;
  for (const auto& interval : coverage) {
    if (interval.first != expected)
      throw std::runtime_error(
        "Quest job history has missing or overlapping identities");
    expected = interval.last + 1;
  }
  if (expected != result.nextJob)
    throw std::runtime_error("Quest job history has missing identities");
  if (version == 5 && !result.live.empty() &&
      result.live.begin()->second.sequenced) {
    // Schema 5 admitted exactly one root and enforced this bound throughout
    // its lifetime. Measure its original canonical shape, before adding
    // environment and interaction fields, so valid near-limit saves migrate.
    size_t bytes = 0;
    for (const auto& job : completed) {
      if (!job.at("sequenced").get<bool>())
        continue;
      const auto size = job.dump().size();
      if (size > MaxRecentBytes - bytes)
        throw std::runtime_error(
          "Legacy sequenced admission exceeds retained completion bytes");
      bytes += size;
    }
  }
  return result;
}

size_t QuestJobState::RetainedSequencedBytes() const
{
  if (completed.size() > MaxCompleted + MaxRecentCompleted)
    throw std::runtime_error("Quest job history limit exceeded");
  size_t total = 0;
  for (const auto& job : completed) {
    if (!job.sequenced)
      continue;
    const auto bytes = SaveJob(job).dump().size();
    if (bytes > std::numeric_limits<size_t>::max() - total)
      throw std::overflow_error(
        "Retained quest completion byte count overflow");
    total += bytes;
  }
  return total;
}

std::vector<uint64_t> QuestJobState::RetireForAdmission()
{
  Save();
  auto count = static_cast<size_t>(
    std::count_if(completed.begin(), completed.end(),
                  [](const Job& job) { return job.sequenced; }));
  for (const auto& [id, job] : live)
    count += job.sequenced ? 1 : 0;
  auto bytes = RetainedSequencedBytes();
  // All allocating work is prepared off to the side. Legacy records and live
  // stacks remain unchanged even if validation or allocation fails.
  auto retained = completed;
  auto ranges = retiredRanges;
  std::vector<uint64_t> retired;
  for (auto current = retained.begin(); current != retained.end() &&
       (count >= MaxRecentCompleted || bytes > MaxRecentBytes);) {
    if (!current->sequenced) {
      ++current;
      continue;
    }
    bytes -= SaveJob(*current).dump().size();
    --count;
    retired.push_back(current->id);
    ranges.push_back({ current->id, current->id });
    current = retained.erase(current);
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<RetiredRange> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.first <= merged.back().last + 1)
      merged.back().last = std::max(merged.back().last, range.last);
    else
      merged.push_back(range);
  }
  // A legacy completion can bridge retired intervals, but the prefix may
  // end only at an actually retired numbered ID, never at a legacy receipt.
  std::vector<RetiredRange> terminal = merged;
  for (const auto& job : retained)
    if (!job.sequenced && job.id > retiredThrough)
      terminal.push_back({ job.id, job.id });
  std::sort(terminal.begin(), terminal.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  auto frontier = retiredThrough;
  auto expected = retiredThrough + 1;
  for (const auto& interval : terminal) {
    if (interval.first != expected)
      break;
    expected = interval.last + 1;
    // Retained legacy IDs were excluded from every sparse range by Read.
    const auto legacy = std::lower_bound(
      retained.begin(), retained.end(), interval.last,
      [](const Job& job, uint64_t id) { return job.id < id; });
    if (legacy == retained.end() || legacy->id != interval.last)
      frontier = interval.last;
  }
  merged.erase(std::remove_if(merged.begin(), merged.end(),
                              [frontier](const auto& range) {
                                return range.last <= frontier;
                              }),
               merged.end());
  completed.swap(retained);
  retiredRanges.swap(merged);
  retiredThrough = frontier;
  return retired;
}

QuestJobState::Job* QuestJobState::FindLive(uint64_t id)
{
  const auto found = live.find(id);
  return found == live.end() ? nullptr : &found->second;
}

const QuestJobState::Job* QuestJobState::FindLive(uint64_t id) const
{
  const auto found = live.find(id);
  return found == live.end() ? nullptr : &found->second;
}

const QuestJobState::Job* QuestJobState::FindCompleted(uint64_t id) const
{
  const auto found = std::lower_bound(
    completed.begin(), completed.end(), id,
    [](const Job& job, uint64_t value) { return job.id < value; });
  return found == completed.end() || found->id != id ? nullptr : &*found;
}

bool QuestJobState::HasLive() const
{
  return !live.empty();
}

bool QuestJobState::IsRetired(uint64_t id) const
{
  if (!id || id >= nextJob || FindLive(id))
    return false;
  if (const auto* completed = FindCompleted(id); completed)
    return false;
  if (id <= retiredThrough)
    return true;
  const auto found =
    std::lower_bound(retiredRanges.begin(), retiredRanges.end(), id,
                     [](const RetiredRange& range, uint64_t value) {
                       return range.last < value;
                     });
  return found != retiredRanges.end() && found->first <= id;
}

nlohmann::json QuestJobState::Save() const
{
  if (completed.size() > MaxCompleted + MaxRecentCompleted ||
      live.size() > MaxLive || retiredRanges.size() > MaxRetiredRanges)
    throw std::runtime_error("Quest job collection limit exceeded");
  auto history = Json::array();
  for (const auto& job : completed)
    history.push_back(SaveJob(job));
  auto current = Json::array();
  for (const auto& [id, job] : live) {
    if (id != job.id)
      throw std::runtime_error("Live quest job map identity mismatch");
    current.push_back(SaveJob(job));
  }
  auto ranges = Json::array();
  for (const auto& range : retiredRanges)
    ranges.push_back({ { "first", range.first }, { "last", range.last } });
  Json result = { { "schemaVersion", 8 },
                  { "generation", generation },
                  { "nextJob", nextJob },
                  { "retiredThrough", retiredThrough },
                  { "retiredRanges", std::move(ranges) },
                  { "live", std::move(current) },
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
  if (generation == 1 && nextJob == 1 && retiredThrough == 0 && live.empty() &&
      retiredRanges.empty() && completed.empty())
    return;
  std::vector<std::pair<const Job*, const Stage*>> waiting;
  for (const auto& [id, job] : live) {
    if (job.cursor == "waiting")
      waiting.emplace_back(&job, &job);
    if (job.handler && job.handler->cursor == "waiting")
      waiting.emplace_back(&job, &*job.handler);
    for (const auto& child : job.children)
      if (child.cursor == "waiting")
        waiting.emplace_back(&job, &child);
  }
  if (continuations.size() != waiting.size())
    throw std::runtime_error("Missing or orphan quest job continuation");
  for (const auto& [job, stage] : waiting) {
    if (!continuations.contains(stage->token))
      throw std::runtime_error("Missing quest stage continuation");
    const auto& continuation = continuations.at(stage->token);
    const auto& callers = continuation.at("callers");
    const auto& outer = callers.empty() ? continuation : callers.back();
    const auto instructions =
      continuation.at("executedInstructions").get<uint64_t>();
    const auto* handler =
      stage->handlerIdentity ? &*stage->handlerIdentity : nullptr;
    const auto receiver = handler ? handler->receiver : Json(stage->quest);
    const auto& root = handler ? handler->rootScript : stage->script;
    const auto& implementation =
      handler ? handler->implementation : stage->script;
    const auto& state = handler ? handler->state : std::string{};
    const auto& function = handler ? handler->function : stage->function;
    if (continuation.at("scope") != job->Scope(*stage) || !instructions ||
        instructions > job->instructions ||
        (stage == &job->Current() && instructions != job->instructions) ||
        !SameScriptName(continuation.at("resultType"),
                        stage->awaitKind == "timer" ? "None" : "Bool") ||
        outer.at("kind") != "method" ||
        !SameScriptName(outer.at("state"), state) ||
        !SameScriptName(outer.at("rootScript"), root) ||
        !SameScriptName(outer.at("script"), implementation) ||
        !SameScriptName(outer.at("function"), function) ||
        outer.at("receiver").at("type") != VarValue::kType_Object ||
        outer.at("receiver").at("value") != receiver)
      throw std::runtime_error(
        "Quest stage continuation does not match its cursor");
  }
}
