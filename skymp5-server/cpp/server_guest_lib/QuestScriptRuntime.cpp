#include "QuestScriptRuntime.h"
#include "CampaignCondition.h"
#include "CampaignFactionApi.h"
#include "CampaignInventoryApi.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceQueryApi.h"
#include "CampaignReferenceScripts.h"
#include "CampaignWorldStateApi.h"
#include "MpActor.h"
#include "QuestJobContent.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
#include "WorldState.h"
#include "libespm/CTDA.h"
#include "libespm/GroupHeader.h"
#include "libespm/QuestAliasData.h"
#include "libespm/QuestLifecycleData.h"
#include "libespm/RecordHeaderAccess.h"
#include "papyrus-vm/Reader.h"
#include "script_classes/PapyrusQuest.h"
#include "script_compatibility_policies/HeuristicPolicy.h"
#include "script_objects/CampaignAliasObject.h"
#include "script_objects/CampaignFormObject.h"
#include "script_objects/CampaignQuestObject.h"
#include "script_objects/MpFormGameObject.h"
#include "script_storages/IScriptStorage.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
using Json = nlohmann::json;
std::string Lower(std::string value)
{
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}

class CampaignGlobalObject final : public EspmGameObject
{
public:
  CampaignGlobalObject(const espm::LookupResult& record, uint32_t owner)
    : EspmGameObject(record)
    , ownerId(owner)
  {
    if (!record.rec || record.rec->GetType() != "GLOB" || !owner)
      throw std::invalid_argument("Expected a campaign global");
    identity = "campaign:" + std::to_string(owner) +
      ":global:" + std::to_string(record.ToGlobalId(record.rec->GetId()));
  }
  uint32_t OwnerId() const { return ownerId; }
  const char* GetStringID() override { return identity.c_str(); }
  bool EqualsByValue(const IGameObject& value) const override
  {
    const auto* other = dynamic_cast<const CampaignGlobalObject*>(&value);
    return other && other->ownerId == ownerId &&
      other->record.rec == record.rec;
  }

private:
  uint32_t ownerId;
  std::string identity;
};

float GlobalDefault(const espm::LookupResult& record, WorldState& world)
{
  std::optional<float> value;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (std::memcmp(tag, "FLTV", 4))
        return;
      if (value || size != sizeof(float))
        throw std::runtime_error("Malformed global value");
      float number;
      std::memcpy(&number, data, sizeof(number));
      if (!std::isfinite(number))
        throw std::runtime_error("Nonfinite global default");
      value = number;
    },
    world.GetEspmCache());
  if (!value)
    throw std::runtime_error("Missing global default");
  return *value;
}

bool Conditional(const PexScript& pex, uint32_t flags)
{
  std::optional<uint8_t> index;
  for (const auto& flag : pex.userFlagTable) {
    if (Lower(flag.name) != "conditional")
      continue;
    if (index || flag.idx >= 32)
      throw std::runtime_error("Invalid conditional user flag");
    index = flag.idx;
  }
  return index && (flags & (uint32_t(1) << *index));
}

uint32_t ReferenceBase(const espm::LookupResult& record, WorldState& world)
{
  if (!record.rec ||
      (record.rec->GetType() != "REFR" && record.rec->GetType() != "ACHR"))
    throw std::invalid_argument("Expected placed reference");
  std::optional<uint32_t> base;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* bytes) {
      if (std::memcmp(tag, "NAME", 4))
        return;
      if (base || size != 4)
        throw std::runtime_error("Malformed reference base record");
      uint32_t value;
      std::memcpy(&value, bytes, 4);
      base = record.ToGlobalId(value);
    },
    world.GetEspmCache());
  if (!base || !*base)
    throw std::runtime_error("Reference has no base object");
  return *base;
}

// INFO ownership is inherited from its DIAL group. SCEN reuses PNAM for
// package actions, so only a QUST target is the scene's owning quest.
uint32_t RecordOwningQuest(espm::LookupResult record, WorldState& world)
{
  auto& browser = world.GetEspm().GetBrowser();
  const bool scene = record.rec->GetType() == "SCEN";
  std::optional<espm::LookupResult> parentTopic;
  if (!scene) {
    if (record.rec->GetType() != "INFO")
      throw std::invalid_argument("Expected scene or topic info");
    std::optional<uint32_t> topic;
    for (const auto* group : browser.GetParentGroupsEnsured(record.rec)) {
      uint32_t raw;
      if (group->GetParentDIAL(raw)) {
        if (topic)
          throw std::runtime_error("Ambiguous topic info parent");
        topic = record.ToGlobalId(raw);
      }
    }
    if (!topic || !*topic)
      throw std::runtime_error("Topic info has no parent topic");
    parentTopic.emplace(browser.LookupById(*topic));
    if (!parentTopic->rec || parentTopic->rec->IsDeleted() ||
        parentTopic->rec->GetType() != "DIAL")
      throw std::runtime_error("Invalid topic info parent");
  }
  const auto& source = parentTopic ? *parentTopic : record;
  std::optional<uint32_t> quest;
  espm::RecordHeaderAccess::IterateFields(
    source.rec,
    [&](const char* tag, uint32_t size, const char* bytes) {
      if (std::memcmp(tag, scene ? "PNAM" : "QNAM", 4))
        return;
      if (size != 4)
        throw std::runtime_error("Malformed owning quest field");
      uint32_t raw;
      std::memcpy(&raw, bytes, 4);
      if (!raw)
        return;
      const auto id = source.ToGlobalId(raw);
      const auto target = browser.LookupById(id);
      if (scene && target.rec && target.rec->GetType() == "PACK")
        return;
      if (!target.rec || target.rec->IsDeleted() ||
          target.rec->GetType() != "QUST")
        throw std::runtime_error("Invalid owning quest target");
      if (quest)
        throw std::runtime_error("Ambiguous owning quest target");
      quest = id;
    },
    world.GetEspmCache());
  return quest.value_or(0);
}

class Variables : public IVariablesHolder
{
public:
  using Resolve = std::function<std::shared_ptr<IGameObject>(uint32_t)>;
  using ResolveAlias =
    std::function<std::shared_ptr<IGameObject>(uint32_t, uint32_t)>;
  Variables(espm::QuestScript script_, espm::LookupResult record_, Json saved_,
            Resolve resolve_, ResolveAlias resolveAlias_, QuestVmState& heap_,
            bool globalPropertyIds_ = false)
    : script(std::move(script_))
    , record(record_)
    , saved(std::move(saved_))
    , resolve(std::move(resolve_))
    , resolveAlias(std::move(resolveAlias_))
    , heap(heap_)
    , globalPropertyIds(globalPropertyIds_)
  {
    if (!saved.is_object())
      throw std::runtime_error("Invalid saved script variables");
    if (!saved.empty()) {
      if (saved.at("schemaVersion") != 1 || !saved.at("values").is_object())
        throw std::runtime_error("Unsupported saved script schema");
      saved = saved.at("values").get<Json>();
    }
  }

  VarValue* GetVariableByName(const char* name, const PexScript& pex) override
  {
    const auto key = Lower(name);
    const auto qualified = Lower(pex.objectTable.at(0).NameIndex) + "/" + key;
    auto it = values.find(qualified);
    if (it != values.end())
      return &it->second;
    VarValue initial;
    bool found = false;
    if (key == "::state") {
      initial = VarValue(pex.objectTable.at(0).autoStateName);
      found = true;
    } else {
      for (const auto& object : pex.objectTable) {
        for (const auto& variable : object.variables) {
          if (Lower(variable.name) != key)
            continue;
          initial = Clone(variable.value);
          if (initial.GetType() == VarValue::kType_Object &&
              !static_cast<IGameObject*>(initial))
            initial =
              VarValue(ActivePexInstance::GetTypeByName(variable.typeName));
          found = true;
        }
      }
      for (const auto& property : script.properties) {
        if ("::" + Lower(property.name) + "_var" != key ||
            property.status == 3)
          continue;
        if (!saved.contains(qualified)) {
          try {
            initial = Convert(property.value);
          } catch (const std::exception& error) {
            throw std::runtime_error(script.name + "." + property.name + ": " +
                                     error.what());
          }
        }
        found = true;
      }
    }
    if (!found)
      return nullptr;
    if (saved.contains(qualified))
      initial = heap.Decode(saved.at(qualified));
    for (const auto& variable : pex.objectTable.at(0).variables)
      if (Lower(variable.name) == key)
        initial.objectType = variable.typeName;
    return &values.emplace(qualified, std::move(initial)).first->second;
  }

  Json Save() const
  {
    auto result = saved;
    for (const auto& [name, value] : values)
      result[name] = heap.Encode(value);
    return { { "schemaVersion", 1 }, { "values", result } };
  }

private:
  static VarValue Clone(const VarValue& value)
  {
    VarValue result = value;
    if (value.pArray) {
      result.pArray = std::make_shared<std::vector<VarValue>>();
      for (const auto& entry : *value.pArray)
        result.pArray->push_back(Clone(entry));
    }
    return result;
  }
  VarValue Convert(const espm::QuestScriptValue& value)
  {
    switch (value.type) {
      case 1:
        if (!value.formId)
          return VarValue::None();
        if (value.alias != 0xffff)
          return VarValue(
            resolveAlias(PropertyId(value.formId), value.alias).get());
        return VarValue(resolve(PropertyId(value.formId)).get());
      case 2:
        return VarValue(value.text);
      case 3:
        return VarValue(value.integer);
      case 4:
        return VarValue(double(value.real));
      case 5:
        return VarValue(value.boolean);
      default:
        if (value.type < 11 || value.type > 15)
          throw std::runtime_error("Unsupported VMAD value");
        VarValue array(value.type);
        array.pArray = std::make_shared<std::vector<VarValue>>();
        for (const auto& element : value.elements)
          array.pArray->push_back(Convert(element));
        return array;
    }
  }
  uint32_t PropertyId(uint32_t raw) const
  {
    return globalPropertyIds ? raw : record.ToGlobalId(raw);
  }
  espm::QuestScript script;
  espm::LookupResult record;
  Json saved;
  Resolve resolve;
  ResolveAlias resolveAlias;
  QuestVmState& heap;
  bool globalPropertyIds;
  std::map<std::string, VarValue> values;
};

struct LogEntry
{
  uint8_t flags = 0;
  std::vector<CampaignCondition> conditions;
};
std::vector<LogEntry> ReadLogs(const espm::LookupResult& record,
                               uint16_t stage, WorldState& world)
{
  bool selected = false;
  bool hasLog = false;
  bool canReadString = false;
  std::vector<LogEntry> logs;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (!std::memcmp(tag, "INDX", 4)) {
        if (size < 2)
          throw std::runtime_error("Malformed stage index");
        uint16_t id;
        std::memcpy(&id, data, sizeof(id));
        selected = id == stage;
        hasLog = canReadString = false;
      } else if (!std::memcmp(tag, "QOBJ", 4) || !std::memcmp(tag, "ANAM", 4))
        selected = false;
      else if (selected && !std::memcmp(tag, "QSDT", 4)) {
        if (size != 1 || (uint8_t(data[0]) & ~1))
          throw std::runtime_error("Unsupported quest log flags");
        logs.push_back({ uint8_t(data[0]), {} });
        hasLog = true;
        canReadString = false;
      } else if (selected && !std::memcmp(tag, "CTDA", 4)) {
        if (!hasLog || size != sizeof(espm::CTDA))
          throw std::runtime_error("Malformed quest log condition");
        espm::CTDA ctda;
        std::memcpy(&ctda, data, sizeof(ctda));
        logs.back().conditions.push_back(CampaignCondition::Compile(
          ctda, [&](uint32_t raw) { return record.ToGlobalId(raw); }));
        canReadString = true;
      } else if (selected &&
                 (!std::memcmp(tag, "CIS1", 4) ||
                  !std::memcmp(tag, "CIS2", 4))) {
        if (!hasLog || !canReadString || logs.back().conditions.empty() ||
            std::memcmp(tag, "CIS2", 4) || size < 2 || size > 65536 ||
            data[size - 1] != 0 || std::memchr(data, 0, size - 1))
          throw std::runtime_error("Invalid quest condition string");
        auto& condition = logs.back().conditions.back();
        condition.SetVariableName(std::string(data, size - 1));
        canReadString = false;
      } else {
        canReadString = false;
      }
    },
    world.GetEspmCache());
  for (const auto& log : logs)
    for (const auto& condition : log.conditions)
      condition.Validate();
  return logs;
}

}

namespace {
struct ReferenceEventContext
{
  MpActor* actor;
  uint32_t target;
  uint32_t source;
};

void RequireInteractionName(const std::string& name)
{
  if (name != "OnActivate" && name != "OnTriggerEnter" &&
      name != "OnTriggerLeave" && name != "OnTrigger" &&
      name != "SkympOnActivateClose")
    throw std::runtime_error("Unsupported event in campaign event dispatcher");
}

ReferenceEventContext ReadInteraction(MpActor* requestedActor,
                                      MpObjectReference& target,
                                      const std::string& name,
                                      MpObjectReference& source)
{
  RequireInteractionName(name);
  auto* world = target.GetParent();
  if (!world || target.IsDeleted() || source.GetParent() != world ||
      world->LookupFormByIdNoLoad(target.GetFormId()).get() != &target ||
      world->LookupFormByIdNoLoad(source.GetFormId()).get() != &source ||
      source.IsDeleted() || !target.SharesWorldInstance(source))
    throw std::runtime_error("Campaign interaction reference is unavailable");
  const auto [owner, original] = CampaignReferenceBinding::Context(target);
  auto* player = source.AsActor();
  const bool playerSource =
    player && (player->GetBaseId() == 7 || player->GetProfileId() >= 0);
  auto* actor = playerSource ? player : owner;
  if (!actor || (requestedActor && requestedActor != actor) ||
      actor->GetParent() != world || actor->IsDeleted() ||
      world->LookupFormByIdNoLoad(actor->GetFormId()).get() != actor ||
      !target.SharesWorldInstance(*actor) ||
      &QuestJournal::Owner(*actor) != owner)
    throw std::runtime_error("Interaction belongs to another campaign actor");
  if (playerSource)
    return { actor, original, 0x14 };
  const auto [sourceOwner, sourceOriginal] =
    CampaignReferenceBinding::Context(source);
  if (sourceOwner != owner)
    throw std::runtime_error("Interaction belongs to another campaign");
  return { actor, original, sourceOriginal };
}

std::vector<std::string> ReferenceTemplates(const MpObjectReference& reference)
{
  const auto* actor = reference.AsActor();
  const auto chain = actor ? actor->GetTemplateChain()
                           : reference.GetChangeForm().templateChain;
  if (chain.size() > 128)
    throw std::runtime_error("Campaign reference template limit exceeded");
  std::vector<std::string> result;
  result.reserve(chain.size());
  for (const auto& entry : chain)
    result.push_back(entry.ToString());
  return result;
}

QuestJobState::Interaction InteractionPayload(
  MpActor& actor, MpObjectReference& target, const std::string& name,
  MpObjectReference& source, const ReferenceEventContext& context)
{
  auto& world = *actor.GetParent();
  const auto physical = [&](const MpObjectReference& reference) {
    return world.espmFiles.empty()
      ? FormDesc(reference.GetFormId(), "").ToString()
      : FormDesc::FromFormId(reference.GetFormId(), world.espmFiles)
          .ToString();
  };
  return { name,
           FormDesc::FromFormId(context.target, world.espmFiles).ToString(),
           FormDesc::FromFormId(context.source, world.espmFiles).ToString(),
           physical(target),
           physical(source),
           ReferenceTemplates(target),
           ReferenceTemplates(source) };
}

ReferenceEventContext ReadReferenceEvent(MpObjectReference& target,
                                         const char* eventName,
                                         const VarValue* arguments,
                                         size_t count)
{
  if (!eventName)
    throw std::runtime_error("Unsupported event in campaign event dispatcher");
  RequireInteractionName(eventName);
  if (count != 1 || !arguments || arguments[0].promise)
    throw std::invalid_argument("Campaign interaction needs one reference");
  const auto* object = arguments[0].GetType() == VarValue::kType_Object
    ? dynamic_cast<MpFormGameObject*>(static_cast<IGameObject*>(arguments[0]))
    : nullptr;
  auto* native = object ? object->GetFormPtrNoLoad() : nullptr;
  auto* source = native ? native->AsObjectReference() : nullptr;
  if (!source)
    throw std::runtime_error("Campaign interaction source is unavailable");
  return ReadInteraction(nullptr, target, eventName, *source);
}

std::vector<PexScript::Lazy> LoadCampaignScripts(WorldState& world)
{
  auto* storage = world.GetScriptStorage();
  if (!storage)
    throw std::runtime_error("Quest execution requires owned PEX storage");
  std::vector<PexScript::Lazy> scripts;
  for (const auto& name : storage->ListScripts(false)) {
    auto cached = std::make_shared<std::shared_ptr<PexScript>>();
    const std::string scriptName(name.begin(), name.end());
    scripts.push_back(
      { scriptName, [cached, storage, scriptName] {
         if (!*cached) {
           const auto bytes = storage->GetScriptPex(scriptName.c_str());
           if (bytes.empty())
             throw std::runtime_error("Missing PEX: " + scriptName);
           const auto parsed =
             Reader(std::vector<std::vector<uint8_t>>{ bytes })
               .GetSourceStructures();
           if (parsed.size() != 1)
             throw std::runtime_error("Invalid PEX: " + scriptName);
           *cached = parsed.front();
           if ((*cached)->objectTable.size() != 1 ||
               Lower((*cached)->objectTable.front().NameIndex) !=
                 Lower(scriptName))
             throw std::runtime_error("PEX script binding mismatch: " +
                                      scriptName);
           (*cached)->source = scriptName;
         }
         return *cached;
       } });
  }
  return scripts;
}
}

std::vector<PexScript::Lazy> QuestScriptRuntime::LoadScripts(WorldState& world)
{
  return LoadCampaignScripts(world);
}

QuestJobState::Interaction QuestScriptRuntime::CaptureInteraction(
  MpActor& actor, MpObjectReference& target, const std::string& name,
  MpObjectReference& source)
{
  const auto context = ReadInteraction(&actor, target, name, source);
  if (!QuestJournal::ReferenceInitialized(actor, context.target))
    throw std::runtime_error(
      "Campaign interaction requires reference initialization");
  return InteractionPayload(actor, target, name, source, context);
}

QuestJobState::Interaction QuestScriptRuntime::CaptureInteraction(
  MpActor& actor, MpObjectReference& target, const std::string& name,
  MpObjectReference& source, const QuestJournal::CaptureContext& snapshot)
{
  snapshot.Validate(actor);
  auto& world = *actor.GetParent();
  const auto targetLifetime = world.LookupFormByIdNoLoad(target.GetFormId());
  const auto sourceLifetime = world.LookupFormByIdNoLoad(source.GetFormId());
  const auto targetEpoch = target.GetWorldInstanceEpoch();
  const auto sourceEpoch = source.GetWorldInstanceEpoch();
  const auto context = ReadInteraction(&actor, target, name, source);
  if (!snapshot.ReferenceInitialized(actor, context.target))
    throw std::runtime_error(
      "Campaign interaction requires reference initialization");
  auto result = InteractionPayload(actor, target, name, source, context);
  snapshot.Validate(actor);
  if (world.LookupFormByIdNoLoad(target.GetFormId()) != targetLifetime ||
      world.LookupFormByIdNoLoad(source.GetFormId()) != sourceLifetime ||
      target.GetWorldInstanceEpoch() != targetEpoch ||
      source.GetWorldInstanceEpoch() != sourceEpoch)
    throw std::runtime_error("Campaign capture endpoint changed");
  return result;
}

void QuestScriptRuntime::ValidateInteraction(
  MpActor& actor, const QuestJobState::Interaction& event)
{
  auto* world = actor.GetParent();
  if (!world || actor.IsDeleted() ||
      world->LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor)
    throw std::runtime_error("Campaign interaction actor is unavailable");
  const auto targetForm = world->LookupFormByIdNoLoad(
    FormDesc::FromString(event.targetReference).ToFormId(world->espmFiles));
  const auto sourceForm = world->LookupFormByIdNoLoad(
    FormDesc::FromString(event.sourceReference).ToFormId(world->espmFiles));
  auto* target = targetForm ? targetForm->AsObjectReference() : nullptr;
  auto* source = sourceForm ? sourceForm->AsObjectReference() : nullptr;
  if (!target || !source ||
      CaptureInteraction(actor, *target, event.name, *source) != event)
    throw std::runtime_error("Saved campaign interaction binding changed");
  if (QuestJournal::HasActiveTransaction()) {
    QuestJournal::ReadInventory(actor, *target);
    // A visiting player is the exact initiating actor, not a bound campaign
    // inventory target. Its script argument is still the owner's PlayerRef.
    if (FormDesc::FromString(event.source).ToFormId(world->espmFiles) != 0x14)
      QuestJournal::ReadInventory(actor, *source);
  }
}

QuestJobState::ReferenceLoad QuestScriptRuntime::CaptureReferenceLoad(
  MpActor& actor, MpObjectReference& target)
{
  auto* world = actor.GetParent();
  if (!world || actor.IsDeleted() || target.GetParent() != world ||
      target.IsDeleted() ||
      world->LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor ||
      world->LookupFormByIdNoLoad(target.GetFormId()).get() != &target ||
      !target.SharesWorldInstance(actor) ||
      &QuestJournal::Owner(actor) != &actor)
    throw std::runtime_error("Campaign load requires its registered owner");
  const auto [owner, original] = CampaignReferenceBinding::Context(target);
  if (owner != &actor)
    throw std::runtime_error("Reference load belongs to another campaign");
  return { FormDesc::FromFormId(original, world->espmFiles).ToString(),
           target.GetChangeForm().formDesc.ToString(),
           ReferenceTemplates(target) };
}

void QuestScriptRuntime::ValidateReferenceLoad(
  MpActor& actor, const QuestJobState::ReferenceLoad& load)
{
  auto* world = actor.GetParent();
  if (!world || actor.IsDeleted() ||
      world->LookupFormByIdNoLoad(actor.GetFormId()).get() != &actor)
    throw std::runtime_error("Campaign load owner is unavailable");
  const auto targetForm = world->LookupFormByIdNoLoad(
    FormDesc::FromString(load.reference).ToFormId(world->espmFiles));
  auto* target = targetForm ? targetForm->AsObjectReference() : nullptr;
  if (!target || CaptureReferenceLoad(actor, *target) != load)
    throw std::runtime_error("Saved campaign load binding changed");
  if (QuestJournal::HasActiveTransaction())
    QuestJournal::ReadInventory(actor, *target);
}

bool QuestScriptRuntime::ExecuteStage(MpActor& actor, uint32_t questId,
                                      uint16_t stage)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign actor");
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  const auto metadata =
    espm::QuestScriptData::Read(record.rec, world.GetEspmCache());
  return ExecuteStage(actor, questId, stage, metadata,
                      LoadCampaignScripts(world));
}

bool QuestScriptRuntime::ExecuteStage(
  MpActor& actor, uint32_t questId, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  return Execute(actor, questId, stage, metadata, scripts);
}

void QuestScriptRuntime::ExecuteReferenceLoad(MpActor& actor,
                                              uint32_t original)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign actor");
  ExecuteReferenceLoad(actor, original,
                       LoadCampaignScripts(*actor.GetParent()));
}

void QuestScriptRuntime::ExecuteReferenceLoad(
  MpActor& actor, uint32_t original,
  const std::vector<PexScript::Lazy>& scripts)
{
  if (!original || original == 0x14 || original >= 0xff000000)
    throw std::invalid_argument("Expected a static campaign reference");
  Execute(actor, 0, 0, {}, scripts, original);
}

void QuestScriptRuntime::ExecuteReferenceEvent(MpObjectReference& target,
                                               const char* eventName,
                                               const VarValue* arguments,
                                               size_t count)
{
  // Validate ingress before loading scripts, including for unsupported events.
  ReadReferenceEvent(target, eventName, arguments, count);
  ExecuteReferenceEvent(target, eventName, arguments, count,
                        LoadCampaignScripts(*target.GetParent()));
}

void QuestScriptRuntime::ExecuteReferenceEvent(
  MpObjectReference& target, const char* eventName, const VarValue* arguments,
  size_t count, const std::vector<PexScript::Lazy>& scripts)
{
  const auto context = ReadReferenceEvent(target, eventName, arguments, count);
  Execute(*context.actor, 0, 0, {}, scripts, context.target, eventName,
          context.source);
}

void QuestScriptRuntime::ExecuteJobSegment(
  MpActor& actor, QuestJobState::Job& job,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts,
  std::optional<QuestJobState::Completion> completion, bool fixtureAwait,
  const std::map<uint32_t, espm::QuestScriptData>& overrides,
  const std::string& contentManifest, const QuestJobClock& clock,
  std::optional<QuestJobTimerAnchor>* timerAnchor)
{
  if (timerAnchor)
    timerAnchor->reset();
  QuestJournal::RequireJobExecution(actor, job);
  const auto jobs = QuestJournal::JobState(actor);
  const auto* selected = jobs.FindLive(job.id);
  if (!selected || selected->generation != job.generation)
    throw std::runtime_error("Quest job does not own this execution segment");
  const auto& current = job.Current();
  auto& world = *actor.GetParent();
  const auto id = current.quest.empty()
    ? 0
    : FormDesc::FromString(current.quest).ToFormId(world.espmFiles);
  uint32_t target = 0, source = 0;
  const char* name = nullptr;
  if (job.kind == QuestJobState::Kind::Interaction) {
    if (!job.interaction)
      throw std::runtime_error("Interaction job has no saved payload");
    ValidateInteraction(actor, *job.interaction);
    target =
      FormDesc::FromString(job.interaction->target).ToFormId(world.espmFiles);
    source =
      FormDesc::FromString(job.interaction->source).ToFormId(world.espmFiles);
    name = job.interaction->name.c_str();
  } else if (job.kind == QuestJobState::Kind::ReferenceLoad) {
    if (!job.referenceLoad)
      throw std::runtime_error("Reference load job has no saved payload");
    ValidateReferenceLoad(actor, *job.referenceLoad);
    target = FormDesc::FromString(job.referenceLoad->target)
               .ToFormId(world.espmFiles);
  }
  Execute(actor, id, current.stage, metadata, scripts, target, name, source,
          &job, completion, fixtureAwait, overrides, contentManifest, clock,
          timerAnchor);
  QuestJournal::CompleteAdmissionCapture(actor, job);
}

bool QuestScriptRuntime::Execute(
  MpActor& actor, uint32_t questId, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts, uint32_t loadedReference,
  const char* referenceEvent, uint32_t eventSource, QuestJobState::Job* job,
  std::optional<QuestJobState::Completion> completion, bool fixtureAwait,
  const std::map<uint32_t, espm::QuestScriptData>& overrides,
  const std::string& contentManifest, const QuestJobClock& clock,
  std::optional<QuestJobTimerAnchor>* timerAnchor)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign actor");
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  if (!loadedReference && (!record.rec || record.rec->GetType() != "QUST"))
    throw std::invalid_argument("Expected quest record");
  // deque keeps this frame stable when a native queues its child. All frames
  // share one VM heap and the root's budgets and pending inventory events.
  auto* work = job ? &job->Current() : nullptr;
  auto readMetadata = [&](uint32_t id) {
    if (const auto found = overrides.find(id); found != overrides.end())
      return found->second;
    if (id == questId)
      return metadata;
    const auto target = world.GetEspm().GetBrowser().LookupById(id);
    if (!target.rec || target.rec->IsDeleted() ||
        target.rec->GetType() != "QUST")
      throw std::invalid_argument("Expected a live quest record");
    return espm::QuestScriptData::Read(target.rec, world.GetEspmCache());
  };
  bool changed = false;
  const auto run = [&] {
    MpObjectReference* loaded = nullptr;
    if (loadedReference) {
      loaded = &CampaignReferenceBinding::Resolve(actor, loadedReference);
      // Read tracking guards the physical copy's lifetime and phase even
      // when a load handler only changes script variables.
      QuestJournal::ReadInventory(actor, *loaded);
      if (referenceEvent) {
        if (!QuestJournal::ReferenceInitialized(actor, loadedReference))
          throw std::runtime_error(
            "Campaign interaction requires reference initialization");
        if (eventSource != 0x14) {
          auto& source = CampaignReferenceBinding::Resolve(actor, eventSource);
          QuestJournal::ReadInventory(actor, source);
        }
      }
    }
    VirtualMachine vm(scripts);
    vm.SetStrictExecution(true);
    bool managedFragment = false;
    if (job)
      vm.RestoreExecutionBudget(job->instructions);
    if (fixtureAwait) {
      if (!job)
        throw std::runtime_error("Fixture await requires a durable job");
      vm.RegisterFunction(
        "FixtureAwait", "AwaitBool", FunctionType::GlobalFunction,
        [&](VarValue, std::vector<VarValue> args) -> VarValue {
          if (args.size() != 1 || args[0].GetType() != VarValue::kType_String)
            throw std::invalid_argument("Fixture await requires a string key");
          const std::string key = static_cast<const char*>(args[0]);
          if (!managedFragment || key.empty() || key.size() > 128 ||
              job->nextOperation > QuestJobState::MaxOperations)
            throw std::runtime_error("Quest await limit exceeded");
          work->token = job->NextToken();
          ++job->nextOperation;
          work->awaitKey = key;
          work->awaitKind = "bool";
          vm.RequestSuspension(work->token, "Bool");
        });
    }
    // The shared server has no global menu pause. Both elapsed-time natives
    // therefore use continuous server time; game-calendar waits stay blocked.
    // Do not register ordinary PapyrusUtility's non-durable Promise timers.
    for (const auto* name : { "Wait", "WaitMenuMode" }) {
      vm.RegisterFunction(
        "Utility", name, FunctionType::GlobalFunction,
        [&, name](VarValue, std::vector<VarValue> args) -> VarValue {
          if (args.size() != 1 || args[0].promise ||
              (args[0].GetType() != VarValue::kType_Float &&
               args[0].GetType() != VarValue::kType_Integer))
            throw std::invalid_argument("Utility wait requires one number");
          const double seconds = args[0].GetType() == VarValue::kType_Integer
            ? static_cast<double>(static_cast<int32_t>(args[0]))
            : static_cast<double>(args[0]);
          if (!std::isfinite(seconds))
            throw std::invalid_argument(
              "Utility wait requires finite seconds");
          // Owned native cores complete nonpositive arguments without queuing
          // a timer. This path is safe in synchronous event/load execution.
          if (seconds <= 0)
            return VarValue::None();
          if (!managedFragment || !job || !work || !clock || !timerAnchor)
            throw std::runtime_error(
              "Positive Utility wait requires a durable timer context");
          if (job->nextOperation > QuestJobState::MaxOperations)
            throw std::runtime_error("Quest timer operation limit exceeded");
          // Native MULSS rounds in float32 before CVTTSS2SI truncates. Clamp
          // first to retain its 24-hour cap without integer overflow/wrapping.
          // Positive sub-millisecond arguments still queue a zero-ms timer.
          const float milliseconds =
            static_cast<float>(std::min(seconds, 86400.0)) * 1000.0f;
          const auto duration = static_cast<uint64_t>(milliseconds);
          const auto now = clock();
          constexpr uint64_t maxUnixMs = 9007199254740991ULL;
          if (now.unixMs > maxUnixMs || duration > maxUnixMs - now.unixMs ||
              duration > std::numeric_limits<uint64_t>::max() - now.steadyMs)
            throw std::overflow_error("Quest timer deadline overflow");
          const auto token = job->NextToken();
          const QuestJobState::Timer timer{ now.unixMs, now.unixMs + duration,
                                            duration };
          const QuestJobTimerAnchor anchor{ token, now.steadyMs + duration };
          work->token = token;
          ++job->nextOperation;
          work->awaitKind = "timer";
          work->awaitKey = std::string("Utility.") + name;
          work->timer = timer;
          *timerAnchor = anchor;
          vm.RequestSuspension(work->token, "None");
        });
    }
    auto policy = std::make_shared<HeuristicPolicy>(&world);
    PapyrusQuest questApi;
    questApi.Register(vm, policy);
    for (const auto* name : { "Start", "Stop", "Reset" }) {
      vm.RegisterFunction(
        "Quest", name, FunctionType::Method,
        [](VarValue, std::vector<VarValue>) -> VarValue {
          throw std::runtime_error(
            "Nested quest lifecycle change requires a lifecycle adapter");
        });
    }
    auto stack =
      std::shared_ptr<StackData>(new StackData{ StackIdHolder(vm) });
    policy->SetDefaultActor(stack->stackIdHolder.GetStackId(), &actor);
    const auto owner = QuestJournal::Owner(actor).GetFormId();
    std::map<uint32_t, std::shared_ptr<IGameObject>> objects;
    std::map<std::string, std::shared_ptr<Variables>> variables;
    std::function<std::shared_ptr<IGameObject>(uint32_t)> resolve;
    std::function<std::shared_ptr<IGameObject>(uint32_t, uint32_t)>
      resolveAlias;
    std::map<uint32_t, espm::QuestAliasData> aliasDefinitions;
    std::map<uint32_t, espm::QuestLifecycleData> lifecycleDefinitions;
    std::map<std::pair<uint32_t, uint32_t>,
             std::shared_ptr<CampaignAliasObject>>
      aliases;
    const auto encodeObject = [&](IGameObject* value) -> Json {
      if (auto* alias = dynamic_cast<CampaignAliasObject*>(value)) {
        if (alias->OwnerId() != owner)
          throw std::runtime_error("Alias belongs to another campaign");
        return { { "kind", "alias" },
                 { "quest",
                   FormDesc::FromFormId(alias->QuestId(), world.espmFiles)
                     .ToString() },
                 { "id", alias->Definition().id } };
      }
      if (auto* reference = dynamic_cast<CampaignFormObject*>(value)) {
        if (reference->OwnerId() != owner)
          throw std::runtime_error("Form belongs to another campaign");
        return FormDesc::FromFormId(reference->FormId(), world.espmFiles)
          .ToString();
      }
      const EspmGameObject* form = nullptr;
      if (auto* quest = dynamic_cast<CampaignQuestObject*>(value)) {
        if (quest->OwnerId() == owner)
          form = quest;
      } else if (auto* global = dynamic_cast<CampaignGlobalObject*>(value)) {
        if (global->OwnerId() == owner)
          form = global;
      }
      if (!form)
        throw std::runtime_error("Unsupported saved campaign object");
      return FormDesc::FromFormId(
               form->record.ToGlobalId(form->record.rec->GetId()),
               world.espmFiles)
        .ToString();
    };
    const auto decodeObject = [&](const Json& value) -> VarValue {
      if (value.is_object()) {
        if (value.size() != 3 || value.at("kind") != "alias" ||
            !value.at("quest").is_string() ||
            !value.at("id").is_number_integer() ||
            value.at("id").get<int64_t>() < 0 ||
            value.at("id").get<uint64_t>() > INT32_MAX)
          throw std::runtime_error("Invalid saved campaign alias identity");
        return VarValue(resolveAlias(FormDesc::FromString(
                                       value.at("quest").get<std::string>())
                                       .ToFormId(world.espmFiles),
                                     value.at("id").get<uint32_t>())
                          .get());
      }
      return VarValue(resolve(FormDesc::FromString(value.get<std::string>())
                                .ToFormId(world.espmFiles))
                        .get());
    };
    QuestVmState state(QuestJournal::ScriptState(actor), encodeObject,
                       decodeObject);

    resolve = [&](uint32_t id) -> std::shared_ptr<IGameObject> {
      if (objects.count(id))
        return objects.at(id);
      const auto rec = world.GetEspm().GetBrowser().LookupById(id);
      if (id != 0x14 && (!rec.rec || rec.rec->IsDeleted()))
        throw std::runtime_error("Missing or deleted campaign form");
      if (rec.rec && rec.rec->GetType() == "GLOB") {
        auto global = std::make_shared<CampaignGlobalObject>(rec, owner);
        objects[id] = global;
        return global;
      }

      if (id == 0x14 || rec.rec->GetType() != "QUST") {
        auto form = std::make_shared<CampaignFormObject>(rec, owner, id);
        objects[id] = form;
        if (id != 0x14 &&
            (rec.rec->GetType() == "REFR" || rec.rec->GetType() == "ACHR")) {
          std::vector<VirtualMachine::ScriptInfo> bindings;
          for (const auto& script :
               CampaignReferenceScripts::Read(world, rec, [&] {
                 auto& copy = CampaignReferenceBinding::Resolve(actor, id);
                 QuestJournal::ReadInventory(actor, copy);
                 return copy.GetChangeForm().templateChain;
               })) {
            const auto pex = vm.GetPexByName(script.name);
            if (!pex.fn)
              throw std::runtime_error("Missing campaign reference script: " +
                                       script.name);
            pex.fn();
            const auto key = "reference:" +
              FormDesc::FromFormId(id, world.espmFiles).ToString() + "/" +
              Lower(script.name);
            auto holder =
              std::make_shared<Variables>(script, rec, state.GetScript(key),
                                          resolve, resolveAlias, state, true);
            variables.emplace(key, holder);
            bindings.push_back({ script.name, holder });
          }
          vm.AddObject(form, bindings);
        }
        return form;
      }
      auto object = std::make_shared<CampaignQuestObject>(rec, owner);
      objects[id] = object;
      const auto data = readMetadata(id);
      std::vector<VirtualMachine::ScriptInfo> bindings;
      for (const auto& script : data.scripts) {
        if (script.status & 2)
          continue;
        // AddObject used to silently ignore missing scripts. Verify each
        // binding.
        const auto pex = vm.GetPexByName(script.name);
        if (!pex.fn)
          throw std::runtime_error("Missing quest script: " + script.name);
        pex.fn();
        const auto key = FormDesc::FromFormId(id, world.espmFiles).ToString() +
          "/" + Lower(script.name);
        if (variables.count(key))
          throw std::runtime_error("Duplicate quest script binding");
        auto holder = std::make_shared<Variables>(
          script, rec, state.GetScript(key), resolve, resolveAlias, state);
        variables[key] = holder;
        bindings.push_back({ script.name, holder });
      }
      vm.AddObject(object, bindings);
      return object;
    };

    auto questKey = [&](uint32_t id) {
      return FormDesc::FromFormId(id, world.espmFiles).ToString();
    };
    auto lifecycle = [&](uint32_t id) -> const espm::QuestLifecycleData& {
      auto it = lifecycleDefinitions.find(id);
      if (it == lifecycleDefinitions.end()) {
        const auto rec = world.GetEspm().GetBrowser().LookupById(id);
        it = lifecycleDefinitions
               .emplace(id, espm::QuestLifecycleData::Read(
                              rec.rec, world.GetEspmCache()))
               .first;
      }
      return it->second;
    };
    auto definitions = [&](uint32_t id) -> const espm::QuestAliasData& {
      auto it = aliasDefinitions.find(id);
      if (it == aliasDefinitions.end()) {
        const auto rec = world.GetEspm().GetBrowser().LookupById(id);
        it = aliasDefinitions
               .emplace(
                 id, espm::QuestAliasData::Read(rec.rec, world.GetEspmCache()))
               .first;
      }
      return it->second;
    };
    resolveAlias = [&](uint32_t id,
                       uint32_t aliasId) -> std::shared_ptr<IGameObject> {
      const auto key = std::make_pair(id, aliasId);
      if (aliases.count(key))
        return aliases.at(key);
      const auto* definition = definitions(id).Find(aliasId);
      if (!definition)
        throw std::runtime_error("Quest alias does not exist");
      auto object =
        std::make_shared<CampaignAliasObject>(owner, id, *definition);
      aliases.emplace(key, object);
      const auto rec = world.GetEspm().GetBrowser().LookupById(id);
      const auto data = readMetadata(id);
      std::vector<VirtualMachine::ScriptInfo> bindings;
      for (const auto& binding : data.aliases) {
        if (binding.object.alias != aliasId)
          continue;
        if (rec.ToGlobalId(binding.object.formId) != id)
          throw std::runtime_error("Alias script is bound to another quest");
        for (const auto& script : binding.scripts) {
          if (script.status & 2)
            continue;
          const auto pex = vm.GetPexByName(script.name);
          if (!pex.fn)
            throw std::runtime_error("Missing alias script: " + script.name);
          pex.fn();
          const auto stateKey = questKey(id) +
            "/alias:" + std::to_string(aliasId) + "/" + Lower(script.name);
          if (variables.count(stateKey))
            throw std::runtime_error("Duplicate alias script binding");
          auto holder =
            std::make_shared<Variables>(script, rec, state.GetScript(stateKey),
                                        resolve, resolveAlias, state);
          variables[stateKey] = holder;
          bindings.push_back({ script.name, holder });
        }
      }
      vm.AddObject(object, bindings);
      return object;
    };

    auto aliasSelf = [&](const VarValue& self) -> CampaignAliasObject& {
      if (self.GetType() != VarValue::kType_Object)
        throw std::invalid_argument("Expected campaign alias");
      auto* object =
        dynamic_cast<CampaignAliasObject*>(static_cast<IGameObject*>(self));
      if (!object || object->OwnerId() != owner)
        throw std::runtime_error("Alias belongs to another campaign");
      return *object;
    };
    auto formId = [&](const VarValue& self) -> uint32_t {
      if (self.GetType() != VarValue::kType_Object)
        throw std::invalid_argument("Expected campaign form");
      auto* object = static_cast<IGameObject*>(self);
      if (const auto* form = dynamic_cast<CampaignFormObject*>(object)) {
        if (form->OwnerId() == owner)
          return form->FormId();
      }
      const EspmGameObject* result = nullptr;
      if (const auto* quest = dynamic_cast<CampaignQuestObject*>(object)) {
        if (quest->OwnerId() == owner)
          result = quest;
      } else if (const auto* global =
                   dynamic_cast<CampaignGlobalObject*>(object)) {
        if (global->OwnerId() == owner)
          result = global;
      }
      if (!result)
        throw std::runtime_error(
          "Form belongs to another campaign or is an alias");
      return result->record.ToGlobalId(result->record.rec->GetId());
    };
    CampaignReferenceQueryApi::Register(vm, actor, formId, resolve);
    CampaignWorldStateApi::Register(vm, actor, formId);
    CampaignFactionApi::Register(vm, actor, formId);
    vm.RegisterFunction("Form", "GetFormID", FunctionType::Method,
                        [&](VarValue self, std::vector<VarValue> args) {
                          if (!args.empty())
                            throw std::invalid_argument(
                              "GetFormID takes no arguments");
                          return VarValue(static_cast<int32_t>(formId(self)));
                        });
    vm.RegisterFunction("Game", "GetPlayer", FunctionType::GlobalFunction,
                        [&](VarValue, std::vector<VarValue> args) {
                          if (!args.empty())
                            throw std::invalid_argument(
                              "GetPlayer takes no arguments");
                          return VarValue(resolve(0x14).get());
                        });
    vm.RegisterFunction(
      "Game", "GetForm", FunctionType::GlobalFunction,
      [&](VarValue, std::vector<VarValue> args) {
        if (args.size() != 1 || args[0].GetType() != VarValue::kType_Integer)
          throw std::invalid_argument("GetForm needs one form ID");
        const auto id = static_cast<uint32_t>(static_cast<int32_t>(args[0]));
        if (id >= 0xff000000)
          throw std::runtime_error(
            "Dynamic form lookup requires campaign reference allocation");
        const auto rec = world.GetEspm().GetBrowser().LookupById(id);
        if (id != 0x14 && (!rec.rec || rec.rec->IsDeleted()))
          return VarValue::None();
        return VarValue(resolve(id).get());
      });
    auto baseObject = [&](VarValue self, std::vector<VarValue> args) {
      if (!args.empty())
        throw std::invalid_argument("Base object getter takes no arguments");
      const auto id = formId(self);
      const auto base = id == 0x14
        ? QuestJournal::Owner(actor).GetBaseId()
        : ReferenceBase(world.GetEspm().GetBrowser().LookupById(id), world);
      return VarValue(resolve(base).get());
    };
    vm.RegisterFunction("ObjectReference", "GetBaseObject",
                        FunctionType::Method, baseObject);
    vm.RegisterFunction("Actor", "GetActorBase", FunctionType::Method,
                        baseObject);

    std::vector<CampaignInventoryApi::Event> inventoryEvents;
    if (job) {
      for (const auto& event : job->events)
        inventoryEvents.push_back(
          { FormDesc::FromString(event.target).ToFormId(world.espmFiles),
            FormDesc::FromString(event.item).ToFormId(world.espmFiles),
            event.count,
            event.container.empty() ? 0
                                    : FormDesc::FromString(event.container)
                                        .ToFormId(world.espmFiles),
            event.added });
    }
    CampaignInventoryApi::Register(vm, actor, formId, [&](auto event) {
      if (inventoryEvents.size() >= 4096)
        throw std::runtime_error("Campaign inventory event limit exceeded");
      inventoryEvents.push_back(event);
    });

    for (const auto& [cls, type] :
         std::vector<std::pair<std::string, std::string>>{
           { "Scene", "SCEN" }, { "TopicInfo", "INFO" } }) {
      vm.RegisterFunction(
        cls, "GetOwningQuest", FunctionType::Method,
        [&, type](VarValue self, std::vector<VarValue> args) {
          const auto record =
            world.GetEspm().GetBrowser().LookupById(formId(self));
          if (!args.empty() || !record.rec ||
              record.rec->GetType().ToString() != type)
            throw std::invalid_argument(
              "Invalid GetOwningQuest receiver or arguments");
          const auto quest = RecordOwningQuest(record, world);
          return quest ? VarValue(resolve(quest).get()) : VarValue::None();
        });
    }

    vm.RegisterFunction(
      "Quest", "GetAlias", FunctionType::Method,
      [&](VarValue self, std::vector<VarValue> args) {
        const auto id = formId(self);
        const auto rec = world.GetEspm().GetBrowser().LookupById(id);
        if (!rec.rec || rec.rec->GetType() != "QUST" || args.size() != 1 ||
            args[0].GetType() != VarValue::kType_Integer)
          throw std::invalid_argument(
            "GetAlias needs a quest and one alias ID");
        const auto aliasId = static_cast<int32_t>(args[0]);
        if (aliasId < 0 ||
            !definitions(id).Find(static_cast<uint32_t>(aliasId)))
          return VarValue::None();
        return VarValue(
          resolveAlias(id, static_cast<uint32_t>(aliasId)).get());
      });
    for (const auto* cls : { "Alias", "ReferenceAlias", "LocationAlias" }) {
      vm.RegisterFunction(
        cls, "GetOwningQuest", FunctionType::Method,
        [&](VarValue self, std::vector<VarValue> args) {
          if (!args.empty())
            throw std::invalid_argument("GetOwningQuest takes no arguments");
          return VarValue(resolve(aliasSelf(self).QuestId()).get());
        });
      vm.RegisterFunction(
        cls, "GetID", FunctionType::Method,
        [&](VarValue self, std::vector<VarValue> args) {
          if (!args.empty())
            throw std::invalid_argument("GetID takes no arguments");
          return VarValue(
            static_cast<int32_t>(aliasSelf(self).Definition().id));
        });
      vm.RegisterFunction(cls, "GetName", FunctionType::Method,
                          [&](VarValue self, std::vector<VarValue> args) {
                            if (!args.empty())
                              throw std::invalid_argument(
                                "GetName takes no arguments");
                            return VarValue(aliasSelf(self).Definition().name);
                          });
    }

    std::optional<std::map<uint32_t, std::vector<uint32_t>>> actorReferences;
    std::set<std::pair<uint32_t, uint32_t>> resolvingAliases;
    std::function<VarValue(CampaignAliasObject&)> aliasTarget;
    aliasTarget = [&](CampaignAliasObject& object) -> VarValue {
      const auto& def = object.Definition();
      const auto id = object.QuestId();
      // A stopped/unstarted quest has no active alias instance.
      if (QuestJournal::Get(actor, id).at("running") != true)
        return VarValue::None();
      QuestJournal::RequireAdapter(actor, id);
      const auto stateKey = questKey(id) + "/alias:" + std::to_string(def.id);
      auto validateTarget = [&](uint32_t target) {
        auto resolved = resolve(target);
        const auto* form = dynamic_cast<CampaignFormObject*>(resolved.get());
        if (!form)
          throw std::runtime_error(
            "Alias target is not a campaign reference/location");
        const bool location =
          form->record.rec && form->record.rec->GetType() == "LCTN";
        const bool reference = form->IsPlayer() ||
          (form->record.rec &&
           (form->record.rec->GetType() == "ACHR" ||
            form->record.rec->GetType() == "REFR"));
        if ((def.kind == espm::QuestAliasDefinition::Kind::Location &&
             !location) ||
            (def.kind == espm::QuestAliasDefinition::Kind::Reference &&
             !reference))
          throw std::runtime_error("Alias target type mismatch");
        return VarValue(resolved.get());
      };
      if (const auto saved = state.GetAliasBinding(stateKey)) {
        if (saved->is_null()) {
          if (!(def.flags & 2))
            throw std::runtime_error(
              "Required alias has an empty saved binding");
          return VarValue::None();
        }
        return validateTarget(FormDesc::FromString(saved->get<std::string>())
                                .ToFormId(world.espmFiles));
      }
      const auto identity = std::make_pair(id, def.id);
      if (resolvingAliases.size() >= 128 ||
          !resolvingAliases.insert(identity).second)
        throw std::runtime_error("Alias resolution cycle or depth limit");
      struct Guard
      {
        std::set<std::pair<uint32_t, uint32_t>>& active;
        std::pair<uint32_t, uint32_t> key;
        ~Guard() { active.erase(key); }
      } guard{ resolvingAliases, identity };
      if (!def.conditions.empty() || !def.unknownFields.empty() ||
          def.Has("ALFI") || def.Has("ALFA") || def.Has("ALCO") ||
          def.Has("ALFE") || def.Has("ALNA"))
        throw std::runtime_error(
          "Alias fill requires lifecycle, world-query or creation support");
      const auto record = world.GetEspm().GetBrowser().LookupById(id);
      std::optional<uint32_t> target;
      const auto forced = def.Unsigned(
        def.kind == espm::QuestAliasDefinition::Kind::Location ? "ALFL"
                                                               : "ALFR");
      const auto unique = def.Unsigned("ALUA");
      const auto external = def.Unsigned("ALEQ");
      if (int(bool(forced)) + int(bool(unique)) + int(bool(external)) != 1)
        throw std::runtime_error(
          "Alias fill is unresolved or has conflicting selectors");
      if (forced) {
        target = record.ToGlobalId(*forced);
      } else if (unique) {
        if (def.kind != espm::QuestAliasDefinition::Kind::Reference)
          throw std::runtime_error("Unique actor used for location alias");
        const auto base = record.ToGlobalId(*unique);
        if (base == 0x7) {
          target = 0x14;
        } else {
          if (!actorReferences) {
            actorReferences.emplace();
            for (const auto& candidate :
                 world.GetEspm().GetBrowser().GetDistinctRecordsByType(
                   "REFR")) {
              // The browser stores ACHR and REFR in its REFR index.
              if (candidate.rec->GetType() == "ACHR" &&
                  !candidate.rec->IsDeleted())
                (*actorReferences)[ReferenceBase(candidate, world)].push_back(
                  candidate.ToGlobalId(candidate.rec->GetId()));
            }
          }
          const auto it = actorReferences->find(base);
          if (it != actorReferences->end()) {
            if (it->second.size() != 1)
              throw std::runtime_error(
                "Unique actor alias has multiple reference candidates");
            target = it->second.front();
          }
        }
      } else {
        const auto aliasId = def.Signed("ALEA");
        if (!aliasId || *aliasId < 0)
          throw std::runtime_error("External alias has no valid alias ID");
        const auto other =
          std::dynamic_pointer_cast<CampaignAliasObject>(resolveAlias(
            record.ToGlobalId(*external), static_cast<uint32_t>(*aliasId)));
        const auto value = aliasTarget(*other);
        if (static_cast<IGameObject*>(value))
          target = formId(value);
      }
      if (!target || !*target) {
        if (!(def.flags & 2))
          throw std::runtime_error("Required alias has no target");
        state.SetAliasBinding(stateKey, nullptr);
        return VarValue::None();
      }
      const auto result = validateTarget(*target);
      state.SetAliasBinding(stateKey, questKey(*target));
      return result;
    };
    for (const auto* method :
         { "GetReference", "GetRef", "GetActorReference", "GetActorRef" }) {
      const std::string name(method);
      vm.RegisterFunction(
        "ReferenceAlias", name, FunctionType::Method,
        [&, name](VarValue self, std::vector<VarValue> args) {
          auto& object = aliasSelf(self);
          if (!args.empty() ||
              object.Definition().kind !=
                espm::QuestAliasDefinition::Kind::Reference)
            throw std::invalid_argument("Expected reference alias getter");
          const auto value = aliasTarget(object);
          if (name == "GetActorReference" || name == "GetActorRef") {
            const auto* form = dynamic_cast<CampaignFormObject*>(
              static_cast<IGameObject*>(value));
            if (!form ||
                (!form->IsPlayer() && form->record.rec->GetType() != "ACHR"))
              return VarValue::None();
          }
          return value;
        });
    }
    vm.RegisterFunction("LocationAlias", "GetLocation", FunctionType::Method,
                        [&](VarValue self, std::vector<VarValue> args) {
                          auto& object = aliasSelf(self);
                          if (!args.empty() ||
                              object.Definition().kind !=
                                espm::QuestAliasDefinition::Kind::Location)
                            throw std::invalid_argument(
                              "Expected location alias getter");
                          return aliasTarget(object);
                        });

    auto globalObject = [&](const VarValue& self) -> CampaignGlobalObject& {
      if (self.GetType() != VarValue::kType_Object)
        throw std::invalid_argument("Expected campaign global self");
      auto* value =
        dynamic_cast<CampaignGlobalObject*>(static_cast<IGameObject*>(self));
      if (!value || value->OwnerId() != owner)
        throw std::runtime_error("Global belongs to another campaign");
      return *value;
    };
    auto globalKey = [&](const CampaignGlobalObject& value) {
      return FormDesc::FromFormId(
               value.record.ToGlobalId(value.record.rec->GetId()),
               world.espmFiles)
        .ToString();
    };
    auto readGlobal = [&](const CampaignGlobalObject& value) {
      const auto saved = state.GetGlobal(globalKey(value));
      return saved ? *saved : GlobalDefault(value.record, world);
    };
    for (const auto* method :
         { "GetValue", "GetValueInt", "SetValue", "SetValueInt", "Mod" }) {
      const std::string name(method);
      vm.RegisterFunction(
        "GlobalVariable", name, FunctionType::Method,
        [&, name](VarValue self, std::vector<VarValue> args) -> VarValue {
          const auto& value = globalObject(self);
          if (name == "GetValue" || name == "GetValueInt") {
            if (!args.empty())
              throw std::invalid_argument("Global getter takes no arguments");
            const auto number = double(readGlobal(value));
            if (name == "GetValue")
              return VarValue(number);
            if (number < INT32_MIN || number > INT32_MAX)
              throw std::runtime_error("Global integer conversion overflow");
            return VarValue(static_cast<int32_t>(number));
          }
          if (args.size() != 1 ||
              (args[0].GetType() != VarValue::kType_Integer &&
               args[0].GetType() != VarValue::kType_Float) ||
              (name == "SetValueInt" &&
               args[0].GetType() != VarValue::kType_Integer))
            throw std::invalid_argument(
              "Global setter needs one numeric value");
          auto number = static_cast<double>(args[0]);
          if (name == "Mod")
            number += readGlobal(value);
          state.SetGlobal(globalKey(value), number);
          return name == "Mod"
            ? VarValue(double(*state.GetGlobal(globalKey(value))))
            : VarValue::None();
        });
    }
    auto readGlobalId = [&](uint32_t id) {
      const auto value =
        std::dynamic_pointer_cast<CampaignGlobalObject>(resolve(id));
      if (!value)
        throw std::runtime_error("Condition expected a campaign global");
      return readGlobal(*value);
    };
    auto readQuestVariable = [&](uint32_t id, const std::string& name) {
      QuestJournal::RequireAdapter(actor, id);
      if (!QuestJournal::Get(actor, id).at("running").get<bool>())
        throw std::runtime_error(
          "Quest variable condition requires a running quest");
      const auto object =
        std::dynamic_pointer_cast<CampaignQuestObject>(resolve(id));
      if (!object)
        throw std::runtime_error("Condition expected a campaign quest");
      const auto data = readMetadata(id);
      std::shared_ptr<PexScript> conditionalScript;
      std::shared_ptr<Variables> holder;
      for (const auto& script : data.scripts) {
        if (script.status & 2)
          continue;
        const auto pex = vm.GetPexByName(script.name).fn();
        if (!Conditional(*pex, pex->objectTable.at(0).userFlags))
          continue;
        if (conditionalScript)
          throw std::runtime_error("Ambiguous conditional quest script");
        conditionalScript = pex;
        holder = variables.at(questKey(id) + "/" + Lower(script.name));
      }
      if (!conditionalScript)
        throw std::runtime_error("Quest has no conditional script");
      std::set<std::string> seen;
      while (conditionalScript) {
        const auto& definition = conditionalScript->objectTable.at(0);
        if (!seen.insert(Lower(definition.NameIndex)).second ||
            seen.size() > 128)
          throw std::runtime_error("Invalid conditional script inheritance");
        for (const auto& variable : definition.variables) {
          if (Lower(variable.name) != Lower(name))
            continue;
          if (!Conditional(*conditionalScript, variable.userFlags))
            throw std::runtime_error("Quest variable is not conditional");
          const auto* value =
            holder->GetVariableByName(name.c_str(), *conditionalScript);
          const auto type = Lower(variable.typeName);
          if (!value ||
              !((type == "int" &&
                 value->GetType() == VarValue::kType_Integer) ||
                (type == "float" &&
                 value->GetType() == VarValue::kType_Float) ||
                (type == "bool" && value->GetType() == VarValue::kType_Bool)))
            throw std::runtime_error(
              "Quest condition variable is not numeric");
          const auto result = type == "bool"
            ? (static_cast<bool>(*value) ? 1.f : 0.f)
            : static_cast<float>(static_cast<double>(*value));
          if (!std::isfinite(result))
            throw std::runtime_error("Nonfinite quest condition variable");
          return result;
        }
        if (definition.parentClassName.empty())
          break;
        const auto parent = vm.GetPexByName(definition.parentClassName);
        if (!parent.fn)
          throw std::runtime_error("Missing conditional script parent");
        conditionalScript = parent.fn();
      }
      throw std::runtime_error("Conditional quest variable does not exist: " +
                               name);
    };
    auto conditionAlias = [&](uint32_t id, uint32_t aliasId) {
      const auto object = std::dynamic_pointer_cast<CampaignAliasObject>(
        resolveAlias(id, aliasId));
      if (!object ||
          object->Definition().kind !=
            espm::QuestAliasDefinition::Kind::Reference)
        throw std::runtime_error("Condition requires a reference alias");
      const auto value = aliasTarget(*object);
      // GetIsAliasRef treats a proven empty alias as a nonmatch. Other
      // physical predicates still reject zero in CampaignCondition::Read.
      if (!static_cast<IGameObject*>(value))
        return uint32_t{ 0 };
      return formId(value);
    };
    size_t depth = 0;
    std::set<std::pair<uint32_t, uint16_t>> activeStages;
    std::function<bool(uint32_t, uint16_t, bool)> execute;
    for (const auto* name : { "SetStage", "SetCurrentStageID" }) {
      vm.RegisterFunction(
        "Quest", name, FunctionType::Method,
        [&](VarValue self, std::vector<VarValue> args) -> VarValue {
          if (self.GetType() != VarValue::kType_Object)
            throw std::invalid_argument("Expected a campaign quest object");
          auto* object = dynamic_cast<CampaignQuestObject*>(
            static_cast<IGameObject*>(self));
          if (!object || object->OwnerId() != owner)
            throw std::runtime_error(
              "Nested stage belongs to another campaign");
          if (args.size() != 1 || args[0].GetType() != VarValue::kType_Integer)
            throw std::invalid_argument("Quest stage must be an integer");
          const auto index = static_cast<int32_t>(args[0]);
          if (index < 0 || index > 65535)
            throw std::invalid_argument("Quest stage out of range");
          const auto targetId =
            object->record.ToGlobalId(object->record.rec->GetId());
          if (managedFragment) {
            // Stop at the native boundary. Entering the child here would put
            // an unserializable C++ native frame below its Papyrus stack.
            QuestJournal::RequireAdapter(actor, targetId);
            if (job->children.size() + 1 >= QuestJobState::MaxDepth ||
                job->nextOperation > QuestJobState::MaxOperations)
              throw std::runtime_error("Quest child work limit exceeded");
            const auto& life = lifecycle(targetId);
            if (std::count_if(life.stages.begin(), life.stages.end(),
                              [&](const auto& entry) {
                                return entry.index == index;
                              }) != 1)
              throw std::runtime_error(
                "Missing or ambiguous child quest stage");
            const auto targetKey =
              FormDesc::FromFormId(targetId, world.espmFiles).ToString();
            const auto reentrant = [&](const QuestJobState::Stage& ancestor) {
              return ancestor.cursor != "done" &&
                ancestor.quest == targetKey && ancestor.stage == index;
            };
            if ((life.flags & espm::QuestLifecycleData::AllowRepeatedStages) &&
                (reentrant(*job) ||
                 std::any_of(job->children.begin(), job->children.end(),
                             reentrant)))
              throw std::runtime_error(
                "Reentrant repeated quest stage requires scheduling");
            QuestJobState::Stage child;
            child.node = job->nextOperation;
            child.quest = targetKey;
            child.stage = static_cast<uint16_t>(index);
            const auto hashManifest = contentManifest.empty()
              ? QuestJobContent::WithMetadataOverrides(
                  world, QuestJobContent::ScriptManifest(scripts), overrides)
              : contentManifest;
            child.contentHash = QuestJobContent::Hash(
              world, targetId, readMetadata(targetId), hashManifest);
            work->token = job->NextToken();
            ++job->nextOperation;
            work->child = child.node;
            work->awaitKey = "quest-stage";
            work->awaitKind = "stage";
            job->children.push_back(std::move(child));
            vm.RequestSuspension(work->token, "Bool");
          }
          return VarValue(
            execute(targetId, static_cast<uint16_t>(index), false));
        });
    }
    // Stage fragments and event handlers share this clean host checkpoint.
    // The native boundary only records intent; it never enters a child VM.
    auto managedCall = [&](const std::shared_ptr<ActivePexInstance>& script,
                           const std::string& selectedState,
                           const std::string& function,
                           const std::vector<VarValue>& args) {
      struct ManagedGuard
      {
        bool& active;
        explicit ManagedGuard(bool& value)
          : active(value)
        {
          active = true;
        }
        ~ManagedGuard() { active = false; }
      } managedGuard(managedFragment);
      PapyrusExecutionResult execution;
      if (work->cursor == "waiting") {
        if (!completion ||
            Lower(work->script) != Lower(script->GetSourcePexName()) ||
            work->function != function || work->child)
          throw std::runtime_error("Saved quest entry mismatch");
        const bool timer = work->awaitKind == "timer";
        if ((!timer && work->awaitKind != "bool" &&
             work->awaitKind != "stage") ||
            completion->kind !=
              (timer ? QuestJobState::Completion::Kind::None
                     : QuestJobState::Completion::Kind::Boolean) ||
            (timer && (completion->value || !work->timer)) ||
            (!timer && work->timer))
          throw std::runtime_error("Quest completion type mismatch");
        const auto saved =
          state.TakeContinuation(job->Scope(*work), work->token);
        if (Lower(saved.resultType) != (timer ? "none" : "bool"))
          throw std::runtime_error("Quest continuation result type mismatch");
        if (!job->completions.emplace(work->token, *completion).second)
          throw std::runtime_error("Quest completion already consumed");
        work->token.clear();
        work->awaitKey.clear();
        work->awaitKind.clear();
        work->timer.reset();
        work->cursor = "begin";
        execution = script->ResumeFunction(
          job->Scope(*work), saved,
          timer ? VarValue::None() : VarValue(completion->value), stack);
      } else {
        work->script = script->GetSourcePexName();
        work->function = function;
        execution = script->StartResumableFunction(
          job->Scope(*work), selectedState, function, args, stack);
      }
      if (execution.continuation) {
        if (work->token != execution.continuation->token ||
            work->awaitKey.empty())
          throw std::runtime_error("Quest await has no saved intent");
        state.PutContinuation(*execution.continuation);
        work->cursor = "waiting";
      }
      return execution;
    };
    execute = [&](uint32_t targetId, uint16_t targetStage, bool managedEntry) {
      if (++depth > 128) {
        --depth;
        throw std::runtime_error("Quest stage recursion limit exceeded");
      }
      struct DepthGuard
      {
        size_t& depth;
        ~DepthGuard() { --depth; }
      } guard{ depth };
      // Native SetStage can also enter from an inventory event after the
      // original stage returned. Recursion depth alone cannot identify the
      // durable outer cursor: only its explicit host entry owns that cursor.
      const bool managed = job && managedEntry;
      const auto target = world.GetEspm().GetBrowser().LookupById(targetId);
      if (!target.rec || target.rec->IsDeleted() ||
          target.rec->GetType() != "QUST")
        throw std::invalid_argument("Expected nested quest record");
      const auto data = readMetadata(targetId);
      const auto& lifecycleData = lifecycle(targetId);
      const auto stageCount = std::count_if(
        lifecycleData.stages.begin(), lifecycleData.stages.end(),
        [&](const auto& definition) { return definition.index == targetStage; });
      if (stageCount != 1)
        throw std::runtime_error(stageCount == 0
                                   ? "Quest stage does not exist"
                                   : "Ambiguous duplicate quest stage index");
      if (job &&
          (lifecycleData.flags &
           espm::QuestLifecycleData::AllowRepeatedStages)) {
        const auto targetKey =
          FormDesc::FromFormId(targetId, world.espmFiles).ToString();
        const auto running = [&](const QuestJobState::Stage& other) {
          // A pristine queued begin has not entered its fragment yet. Event
          // handlers carry a root quest/stage anchor, not a stage invocation.
          return other.cursor == "waiting" && other.quest == targetKey &&
            other.stage == targetStage;
        };
        for (const auto& [otherId, other] : QuestJournal::JobState(actor).live)
          if (otherId != job->id &&
              (running(other) ||
               std::any_of(other.children.begin(), other.children.end(),
                           running)))
            throw std::runtime_error(
              "Repeated quest stage is executing in another live root");
      }
      const auto logs = ReadLogs(target, targetStage, world);
      std::set<uint32_t> seen;
      for (const auto& fragment : data.fragments) {
        if (fragment.stage != targetStage)
          continue;
        if (fragment.logEntry >= logs.size() ||
            !seen.insert(fragment.logEntry).second)
          throw std::runtime_error(
            "Invalid or duplicate quest fragment log binding");
      }
      const auto identity = std::make_pair(targetId, targetStage);
      if (!(managed && work->cursor == "waiting") &&
          !QuestJournal::Apply(actor,
                               { { "quest", targetId },
                                 { "op", "setStage" },
                                 { "index", targetStage } })) {
        if (!(lifecycleData.flags &
              espm::QuestLifecycleData::AllowRepeatedStages))
          return false;
        // Sequential repeat visits are intentional. Reentering a still-active
        // stage needs queued/latent execution, which this synchronous runner
        // cannot reproduce. Refuse it rather than dropping or duplicating work.
        if (activeStages.count(identity))
          throw std::runtime_error(
            "Reentrant repeated quest stage requires scheduling");
        QuestJournal::RecordRepeatedStage(actor, targetId, targetStage);
      }
      // The native dispatcher records a valid stage even when it has no log
      // items, but returns false and has no fragment work to dispatch.
      if (logs.empty())
        return false;
      activeStages.insert(identity);
      struct ActiveStageGuard
      {
        std::set<std::pair<uint32_t, uint16_t>>& active;
        std::pair<uint32_t, uint16_t> identity;
        ~ActiveStageGuard() { active.erase(identity); }
      } activeStageGuard{ activeStages, identity };
      auto object =
        std::dynamic_pointer_cast<CampaignQuestObject>(resolve(targetId));
      if (managed && work->cursor == "waiting" && work->log >= logs.size())
        throw std::runtime_error("Saved quest log is out of range");
      for (size_t logIndex = managed ? work->log : 0; logIndex < logs.size();
           ++logIndex) {
        if (job && ++job->steps > QuestJobState::MaxSteps)
          throw std::runtime_error("Quest job work limit exceeded");
        const bool resuming = managed && work->cursor == "waiting";
        if (!resuming) {
          const bool passes =
            CampaignCondition::Evaluate(logs[logIndex].conditions,
                                        { actor, targetId, readGlobalId,
                                          readQuestVariable, conditionAlias });
          if (!passes) {
            if (managed)
              work->log = static_cast<uint32_t>(logIndex + 1);
            continue;
          }
        }
        // QSDT completion precedes the fragment. Its IsCompleted/IsActive
        // queries must observe that transition in this same transaction.
        if (!resuming && (logs[logIndex].flags & 1))
          QuestJournal::Apply(actor,
                              { { "quest", targetId }, { "op", "complete" } });
        bool foundFragment = false;
        for (const auto& fragment : data.fragments) {
          if (fragment.stage != targetStage || fragment.logEntry != logIndex)
            continue;
          foundFragment = true;
          const auto script = object->Script(fragment.script);
          const auto function =
            script->GetFunctionByName(fragment.function.c_str(), "");
          if (!function.valid || function.IsNative())
            throw std::runtime_error("Missing quest stage fragment: " +
                                     fragment.function);
          std::vector<VarValue> args;
          VarValue result;
          try {
            if (managed) {
              if (resuming &&
                  (Lower(work->script) != Lower(fragment.script) ||
                   work->function != fragment.function))
                throw std::runtime_error("Saved quest fragment mismatch");
              const auto execution =
                managedCall(script, "", fragment.function, {});
              if (execution.continuation) {
                work->log = static_cast<uint32_t>(logIndex);
                return true;
              }
              result = execution.value;
            } else
              result = script->StartFunction(function, args, stack);
          } catch (const std::exception& error) {
            throw std::runtime_error(fragment.script + "." +
                                     fragment.function + ": " + error.what());
          }
          if (result.promise)
            throw std::runtime_error(
              "Latent quest fragment requires durable continuation support");
        }
        if (resuming && !foundFragment)
          throw std::runtime_error("Saved quest fragment binding disappeared");
        if (managed)
          work->log = static_cast<uint32_t>(logIndex + 1);
      }
      return true;
    };
    auto eventRecipients = [&](uint32_t targetId, bool includeAliases) {
      vm.CheckExecutionBudget();
      std::vector<std::shared_ptr<IGameObject>> recipients{ resolve(
        targetId) };
      // Only aliases with attached scripts need event delivery. Unsupported
      // alias filling must fail the transaction rather than omit a handler.
      const auto eventJournal = QuestJournal::Snapshot(actor);
      for (const auto& [key, progress] : eventJournal.at("quests").items()) {
        if (!includeAliases || progress.at("running") != true)
          continue;
        const auto id = FormDesc::FromString(key).ToFormId(world.espmFiles);
        QuestJournal::RequireAdapter(actor, id);
        const auto record = world.GetEspm().GetBrowser().LookupById(id);
        const auto data = readMetadata(id);
        std::set<uint32_t> seen;
        for (const auto& binding : data.aliases) {
          if (std::none_of(
                binding.scripts.begin(), binding.scripts.end(),
                [](const auto& script) { return !(script.status & 2); }) ||
              !seen.insert(binding.object.alias).second)
            continue;
          const auto* definition = definitions(id).Find(binding.object.alias);
          if (!definition)
            throw std::runtime_error("Reference event alias does not exist");
          if (definition->kind != espm::QuestAliasDefinition::Kind::Reference)
            continue;
          auto alias = std::dynamic_pointer_cast<CampaignAliasObject>(
            resolveAlias(id, binding.object.alias));
          const auto target = aliasTarget(*alias);
          if (static_cast<IGameObject*>(target) &&
              formId(target) == targetId) {
            if (recipients.size() >= QuestJobState::MaxRecipients)
              throw std::runtime_error("Quest event recipient limit exceeded");
            recipients.push_back(alias);
          }
        }
      }
      if (recipients.size() > QuestJobState::MaxRecipients)
        throw std::runtime_error("Quest event recipient limit exceeded");
      return recipients;
    };
    auto dispatchReferenceEvent = [&](uint32_t targetId, const char* name,
                                      const std::vector<VarValue>& args,
                                      bool includeAliases) {
      for (const auto& recipient : eventRecipients(targetId, includeAliases))
        vm.SendEventSynchronous(recipient, name, args, stack);
    };
    auto eventArguments = [&](const CampaignInventoryApi::Event& event) {
      return std::vector<VarValue>{
        VarValue(resolve(event.item).get()),
        VarValue(static_cast<int32_t>(event.count)), VarValue::None(),
        event.container ? VarValue(resolve(event.container).get())
                        : VarValue::None()
      };
    };
    // Primary interaction/load events and later inventory notifications share
    // the saved dispatcher. Each load phase captures its own recipient roster;
    // retiring a primary event never consumes an inventory FIFO position.
    const auto* primaryName = job ? job->PrimaryEventName() : nullptr;
    const bool primaryInteraction =
      primaryName && job->kind == QuestJobState::Kind::Interaction;
    const bool primaryLoad =
      primaryName && job->kind == QuestJobState::Kind::ReferenceLoad;
    const auto dispatchName = [&]() -> const char* {
      return primaryName
        ? primaryName
        : (inventoryEvents.at(job->eventHead).added ? "OnItemAdded"
                                                    : "OnItemRemoved");
    };
    const auto dispatchArguments = [&]() {
      if (primaryLoad)
        return std::vector<VarValue>{};
      return primaryInteraction
        ? std::vector<VarValue>{ VarValue(resolve(eventSource).get()) }
        : eventArguments(inventoryEvents.at(job->eventHead));
    };
    // Resolve canonical receivers through this transaction's fresh object set.
    // Membership was selected once; an alias may legitimately change its
    // target while an earlier handler is running.
    auto dispatchObject = [&](const Json& receiver) {
      if (receiver.is_object()) {
        const auto id =
          FormDesc::FromString(receiver.at("quest").get<std::string>())
            .ToFormId(world.espmFiles);
        QuestJournal::RequireAdapter(actor, id);
        return resolveAlias(id, receiver.at("id").get<uint32_t>());
      }
      return resolve(FormDesc::FromString(receiver.get<std::string>())
                       .ToFormId(world.espmFiles));
    };
    auto roster = [&](QuestJobState::Dispatch& dispatch, bool capture) {
      const auto& identity = dispatch.recipients.at(dispatch.recipient);
      const auto recipient = dispatchObject(identity);
      const auto roots = vm.GetEventAttachments(recipient);
      if (roots.size() > QuestJobState::MaxAttachments)
        throw std::runtime_error("Quest event attachment limit exceeded");
      std::vector<std::string> names, templates;
      std::string reference;
      for (const auto& root : roots) {
        if (!root || !root->IsValid())
          throw std::runtime_error("Invalid quest event attachment");
        names.push_back(root->GetSourcePexName());
      }
      if (identity.is_string()) {
        const auto original = FormDesc::FromString(identity.get<std::string>())
                                .ToFormId(world.espmFiles);
        auto& copy = CampaignReferenceBinding::Resolve(actor, original);
        QuestJournal::ReadInventory(actor, copy);
        reference = copy.GetChangeForm().formDesc.ToString();
        for (const auto& selected : copy.GetChangeForm().templateChain)
          templates.push_back(selected.ToString());
      }
      if (capture) {
        dispatch.roots = std::move(names);
        dispatch.templates = std::move(templates);
        dispatch.reference = std::move(reference);
        dispatch.captured = true;
      } else if (dispatch.roots != names || dispatch.templates != templates ||
                 dispatch.reference != reference)
        throw std::runtime_error("Quest event recipient binding changed");
      return roots;
    };
    // A child stage is still executing on behalf of the saved handler. Its
    // effects cannot commit against a removed alias adapter or a replaced
    // physical receiver while the parent is suspended.
    if (job && job->handler && !job->children.empty())
      roster(*job->dispatch, false);
    bool executedWork = false;
    if (job && job->cursor == "done" && job->children.empty()) {
      if (++job->steps > QuestJobState::MaxSteps)
        throw std::runtime_error("Quest job work limit exceeded");
      if (job->handler) {
        auto& dispatch = *job->dispatch;
        const auto roots = roster(dispatch, false);
        if (job->handler->cursor == "done") {
          // Retiring an already durable handler is a host step, never a
          // fabricated Papyrus completion. Do not rerun its prefix or suffix.
          job->handler.reset();
          ++dispatch.attachment;
          work = job;
        } else {
          const auto& identity = *job->handler->handlerIdentity;
          auto implementation = roots.at(dispatch.attachment);
          size_t depth = 0;
          while (implementation && implementation->IsValid() &&
                 Lower(implementation->GetSourcePexName()) !=
                   Lower(identity.implementation)) {
            if (++depth > 128)
              throw std::runtime_error(
                "Quest event inheritance limit exceeded");
            implementation = implementation->GetParentInstance();
          }
          if (!implementation || !implementation->IsValid())
            throw std::runtime_error(
              "Saved quest handler implementation is missing");
          const auto result =
            managedCall(implementation, identity.state, identity.function,
                        dispatchArguments());
          if (result.value.promise)
            throw std::runtime_error(
              "Quest handler returned an unmanaged promise");
          changed = true;
          executedWork = true;
        }
      } else if (!job->dispatch) {
        if (primaryLoad && job->loadPhase == QuestJobState::LoadPhase::Init &&
            QuestJournal::ReferenceInitialized(actor, loadedReference)) {
          // A completed OnInit is permanent for this campaign reference. The
          // durable skip is a host boundary, with no script instructions.
          job->loadPhase = QuestJobState::LoadPhase::CellLoad;
        } else {
          const auto target = primaryName
            ? loadedReference
            : inventoryEvents.at(job->eventHead).target;
          const bool includeAliases =
            !primaryLoad || job->loadPhase != QuestJobState::LoadPhase::Init;
          QuestJobState::Dispatch dispatch;
          for (const auto& recipient : eventRecipients(target, includeAliases))
            dispatch.recipients.push_back(encodeObject(recipient.get()));
          job->dispatch = std::move(dispatch);
        }
      } else {
        auto& dispatch = *job->dispatch;
        if (dispatch.recipient == dispatch.recipients.size()) {
          if (primaryLoad && job->loadPhase == QuestJobState::LoadPhase::Init)
            QuestJournal::MarkReferenceInitialized(actor, loadedReference);
          job->dispatch.reset();
          if (primaryInteraction)
            job->interactionDone = true;
          else if (primaryLoad) {
            switch (job->loadPhase) {
              case QuestJobState::LoadPhase::Init:
                job->loadPhase = QuestJobState::LoadPhase::CellLoad;
                break;
              case QuestJobState::LoadPhase::CellLoad:
                job->loadPhase = QuestJobState::LoadPhase::Load;
                break;
              case QuestJobState::LoadPhase::Load:
                job->loadPhase = QuestJobState::LoadPhase::Done;
                break;
              default:
                throw std::runtime_error("Invalid reference load phase");
            }
          } else
            ++job->eventHead;
        } else if (!dispatch.captured) {
          roster(dispatch, true);
        } else {
          const auto roots = roster(dispatch, false);
          if (dispatch.attachment == roots.size()) {
            ++dispatch.recipient;
            dispatch.attachment = 0;
            dispatch.captured = false;
            dispatch.roots.clear();
            dispatch.templates.clear();
            dispatch.reference.clear();
          } else {
            const auto selected = vm.SelectEventHandler(
              roots.at(dispatch.attachment), dispatchName());
            if (!selected) {
              ++dispatch.attachment;
            } else {
              if (job->nextHandler > QuestJobState::MaxHandlers)
                throw std::runtime_error("Quest event handler limit exceeded");
              QuestJobState::Stage handler;
              handler.quest = job->quest;
              handler.contentHash = job->contentHash;
              handler.stage = job->stage;
              QuestJobState::HandlerIdentity identity;
              identity.receiver = dispatch.recipients.at(dispatch.recipient);
              identity.rootScript = selected->root->GetSourcePexName();
              identity.implementation =
                selected->implementation->GetSourcePexName();
              identity.state = selected->state;
              identity.function = selected->function;
              identity.sequence = job->nextHandler++;
              handler.handlerIdentity = std::move(identity);
              job->handler = std::move(handler);
            }
          }
        }
      }
    } else if (referenceEvent && !job) {
      dispatchReferenceEvent(loadedReference, referenceEvent,
                             { VarValue(resolve(eventSource).get()) }, true);
      changed = true;
    } else if (loadedReference && !job) {
      if (!QuestJournal::ReferenceInitialized(actor, loadedReference)) {
        // An alias's OnInit belongs to quest startup, not its target's load.
        dispatchReferenceEvent(loadedReference, "OnInit", {}, false);
        QuestJournal::MarkReferenceInitialized(actor, loadedReference);
      }
      dispatchReferenceEvent(loadedReference, "OnCellLoad", {}, true);
      dispatchReferenceEvent(loadedReference, "OnLoad", {}, true);
      changed = true;
    } else {
      changed = execute(questId, stage, job != nullptr);
      executedWork = job != nullptr;
      if (!changed && !job)
        return;
    }
    const bool waiting = work && work->cursor == "waiting";
    // Durable jobs drain the root FIFO in later receipt-gated host steps.
    // Ordinary synchronous entry points retain their atomic delivery policy.
    for (size_t eventIndex = 0; !job && eventIndex < inventoryEvents.size();
         ++eventIndex) {
      // Copy before dispatch: handlers may append more inventory events.
      const auto event = inventoryEvents[eventIndex];
      const auto* name = event.added ? "OnItemAdded" : "OnItemRemoved";
      dispatchReferenceEvent(event.target, name, eventArguments(event), true);
    }
    if (loadedReference &&
        &CampaignReferenceBinding::Resolve(actor, loadedReference) != loaded)
      throw std::runtime_error(
        "Campaign load reference changed during execution");
    if (job && job->kind == QuestJobState::Kind::Interaction)
      ValidateInteraction(actor, *job->interaction);
    else if (job && job->kind == QuestJobState::Kind::ReferenceLoad)
      ValidateReferenceLoad(actor, *job->referenceLoad);
    for (const auto& [key, holder] : variables)
      state.SetScript(key, holder->Save());
    QuestJournal::SetScriptState(actor, state.Save());
    if (job) {
      job->instructions = vm.GetExecutedInstructions();
      if (executedWork)
        work->accepted = changed;
      for (size_t index = job->events.size(); index < inventoryEvents.size();
           ++index) {
        const auto& event = inventoryEvents[index];
        job->events.push_back(
          { FormDesc::FromFormId(event.target, world.espmFiles).ToString(),
            FormDesc::FromFormId(event.item, world.espmFiles).ToString(),
            event.container
              ? FormDesc::FromFormId(event.container, world.espmFiles)
                  .ToString()
              : std::string(),
            event.count, event.added });
      }
      if (executedWork && !waiting) {
        work->cursor = "done";
        work->log = 0;
        work->script.clear();
        work->function.clear();
        work->token.clear();
        work->awaitKey.clear();
        work->awaitKind.clear();
        work->timer.reset();
        work->failure.clear();
      }
    }
  };
  if (job)
    run();
  else
    QuestJournal::Atomic(actor, run);
  return changed;
}
