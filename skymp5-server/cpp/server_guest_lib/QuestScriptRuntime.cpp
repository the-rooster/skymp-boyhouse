#include "QuestScriptRuntime.h"
#include "CampaignInventoryApi.h"
#include "CampaignReferenceBinding.h"
#include "CampaignReferenceScripts.h"
#include "ConditionsEvaluator.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
#include "WorldState.h"
#include "condition_functions/ConditionFunctionFactory.h"
#include "libespm/CTDA.h"
#include "libespm/GroupHeader.h"
#include "libespm/QuestAliasData.h"
#include "libespm/RecordHeaderAccess.h"
#include "papyrus-vm/Reader.h"
#include "script_classes/PapyrusQuest.h"
#include "script_compatibility_policies/HeuristicPolicy.h"
#include "script_objects/CampaignAliasObject.h"
#include "script_objects/CampaignFormObject.h"
#include "script_objects/CampaignQuestObject.h"
#include "script_storages/IScriptStorage.h"
#include <cmath>
#include <cstring>

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

class CampaignGlobalCondition final : public ConditionFunction
{
public:
  explicit CampaignGlobalCondition(std::function<float(uint32_t)> read)
    : read(std::move(read))
  {
  }
  const char* GetName() const override { return "GetGlobalValue"; }
  uint16_t GetFunctionIndex() const override { return 74; }
  float Execute(MpActor&, uint32_t id, uint32_t,
                const ConditionEvaluatorContext&) override
  {
    return read(id);
  }

private:
  std::function<float(uint32_t)> read;
};

class CampaignVariableCondition final : public ConditionFunction
{
public:
  using Read = std::function<float(uint32_t, const std::string&)>;
  explicit CampaignVariableCondition(Read read)
    : read(std::move(read))
  {
  }
  const char* GetName() const override { return "GetVMQuestVariable"; }
  uint16_t GetFunctionIndex() const override { return 629; }
  float Execute(MpActor&, uint32_t, uint32_t,
                const ConditionEvaluatorContext&) override
  {
    throw std::runtime_error("Quest variable condition requires CIS2");
  }
  float Execute(MpActor&, const Condition& condition,
                const ConditionEvaluatorContext&) override
  {
    // ReadLogs supplies a resolved FormID and the owned CIS2 variable name.
    return read(std::stoul(condition.parameter1, nullptr, 16),
                condition.parameter2);
  }

private:
  Read read;
};

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
  std::vector<Condition> conditions;
  std::map<size_t, uint32_t> comparisonGlobals;
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
        // Only implemented quest predicates and explicitly resolved player
        // scope.
        if ((ctda.operatorFlag & 0x1a) ||
            static_cast<int>(ctda.GetOperator()) > 5 ||
            (ctda.functionIndex != 56 && ctda.functionIndex != 58 &&
             ctda.functionIndex != 59 && ctda.functionIndex != 543 &&
             ctda.functionIndex != 74 && ctda.functionIndex != 629) ||
            (ctda.runOnType != espm::CTDA::RunOnTypeFlags::Subject &&
             !(ctda.runOnType == espm::CTDA::RunOnTypeFlags::Reference &&
               record.ToGlobalId(ctda.reference) == 0x14)))
          throw std::runtime_error(
            "Unsupported quest log condition or run-on context");
        auto condition = Condition::FromCtda(ctda);
        condition.runsOn = "Subject";
        condition.parameter1 = fmt::format(
          "0x{:x}", record.ToGlobalId(ctda.GetDefaultData().firstParameter));
        if (ctda.operatorFlag & 4) {
          uint32_t raw;
          std::memcpy(&raw, &ctda.comparisonValue, sizeof(raw));
          if (!raw)
            throw std::runtime_error("Quest comparison global is null");
          logs.back().comparisonGlobals.emplace(logs.back().conditions.size(),
                                                record.ToGlobalId(raw));
          condition.value = 0; // Resolved from the transaction at evaluation.
        } else if (!std::isfinite(condition.value)) {
          throw std::runtime_error("Nonfinite quest condition comparison");
        }
        if (ctda.functionIndex == 629)
          condition.parameter2.clear(); // The CTDA slot is not a string ID.
        logs.back().conditions.push_back(std::move(condition));
        canReadString = true;
      } else if (selected &&
                 (!std::memcmp(tag, "CIS1", 4) ||
                  !std::memcmp(tag, "CIS2", 4))) {
        if (!hasLog || !canReadString || logs.back().conditions.empty() ||
            std::memcmp(tag, "CIS2", 4) || size < 2 || size > 65536 ||
            data[size - 1] != 0 || std::memchr(data, 0, size - 1))
          throw std::runtime_error("Invalid quest condition string");
        auto& condition = logs.back().conditions.back();
        if (condition.function != "GetVMQuestVariable" ||
            !condition.parameter2.empty())
          throw std::runtime_error("Unexpected quest condition string");
        condition.parameter2.assign(data, size - 1);
        canReadString = false;
      } else {
        canReadString = false;
      }
    },
    world.GetEspmCache());
  for (const auto& log : logs)
    for (const auto& condition : log.conditions)
      if (condition.function == "GetVMQuestVariable" &&
          condition.parameter2.empty())
        throw std::runtime_error("Missing quest condition variable name");
  return logs;
}

}

namespace {
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

bool QuestScriptRuntime::Execute(MpActor& actor, uint32_t questId,
                                 uint16_t stage,
                                 const espm::QuestScriptData& metadata,
                                 const std::vector<PexScript::Lazy>& scripts,
                                 uint32_t loadedReference)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign actor");
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  if (!loadedReference && (!record.rec || record.rec->GetType() != "QUST"))
    throw std::invalid_argument("Expected quest record");
  bool changed = false;
  QuestJournal::Atomic(actor, [&] {
    MpObjectReference* loaded = nullptr;
    if (loadedReference) {
      loaded = &CampaignReferenceBinding::Resolve(actor, loadedReference);
      // Read tracking guards the physical copy's lifetime and phase even
      // when a load handler only changes script variables.
      QuestJournal::ReadInventory(actor, *loaded);
    }
    VirtualMachine vm(scripts);
    vm.SetStrictExecution(true);
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
    std::map<std::pair<uint32_t, uint32_t>,
             std::shared_ptr<CampaignAliasObject>>
      aliases;
    QuestVmState state(
      QuestJournal::ScriptState(actor),
      [&](IGameObject* value) -> Json {
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
      },
      [&](const Json& value) -> VarValue {
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
      });

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
      const auto data = id == questId
        ? metadata
        : espm::QuestScriptData::Read(rec.rec, world.GetEspmCache());
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
      const auto data = id == questId
        ? metadata
        : espm::QuestScriptData::Read(rec.rec, world.GetEspmCache());
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
      const auto data = id == questId
        ? metadata
        : espm::QuestScriptData::Read(object->record.rec,
                                      world.GetEspmCache());
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
    auto conditionFunctions =
      ConditionFunctionFactory::CreateConditionFunctions();
    conditionFunctions.RegisterConditionFunction(
      std::make_shared<CampaignGlobalCondition>(readGlobalId));
    conditionFunctions.RegisterConditionFunction(
      std::make_shared<CampaignVariableCondition>(readQuestVariable));
    size_t depth = 0;
    std::function<bool(uint32_t, uint16_t)> execute;
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
          return VarValue(
            execute(object->record.ToGlobalId(object->record.rec->GetId()),
                    static_cast<uint16_t>(index)));
        });
    }
    execute = [&](uint32_t targetId, uint16_t targetStage) {
      if (++depth > 128) {
        --depth;
        throw std::runtime_error("Quest stage recursion limit exceeded");
      }
      struct DepthGuard
      {
        size_t& depth;
        ~DepthGuard() { --depth; }
      } guard{ depth };
      const auto target = world.GetEspm().GetBrowser().LookupById(targetId);
      if (!target.rec || target.rec->GetType() != "QUST")
        throw std::invalid_argument("Expected nested quest record");
      const auto data = targetId == questId
        ? metadata
        : espm::QuestScriptData::Read(target.rec, world.GetEspmCache());
      const auto logs = ReadLogs(target, targetStage, world);
      if (!QuestJournal::Apply(actor,
                               { { "quest", targetId },
                                 { "op", "setStage" },
                                 { "index", targetStage } }))
        return false;
      auto object =
        std::dynamic_pointer_cast<CampaignQuestObject>(resolve(targetId));

      std::set<uint32_t> seen;
      for (const auto& fragment : data.fragments) {
        if (fragment.stage != targetStage)
          continue;
        if (fragment.logEntry >= logs.size() ||
            !seen.insert(fragment.logEntry).second)
          throw std::runtime_error(
            "Invalid or duplicate quest fragment log binding");
      }
      for (size_t logIndex = 0; logIndex < logs.size(); ++logIndex) {
        auto conditions = logs[logIndex].conditions;
        for (const auto& [index, global] : logs[logIndex].comparisonGlobals)
          conditions.at(index).value = readGlobalId(global);
        bool passes = conditions.empty();
        if (!passes)
          ConditionsEvaluator::EvaluateConditions(
            conditionFunctions, {}, ConditionsEvaluatorCaller::kCraft,
            conditions, actor, actor,
            [&](bool result, auto&) { passes = result; });
        if (!passes)
          continue;
        for (const auto& fragment : data.fragments) {
          if (fragment.stage != targetStage || fragment.logEntry != logIndex)
            continue;
          const auto script = object->Script(fragment.script);
          const auto function =
            script->GetFunctionByName(fragment.function.c_str(), "");
          if (!function.valid || function.IsNative())
            throw std::runtime_error("Missing quest stage fragment: " +
                                     fragment.function);
          std::vector<VarValue> args;
          VarValue result;
          try {
            result = script->StartFunction(function, args, stack);
          } catch (const std::exception& error) {
            throw std::runtime_error(fragment.script + "." +
                                     fragment.function + ": " + error.what());
          }
          if (result.promise)
            throw std::runtime_error(
              "Latent quest fragment requires durable continuation support");
        }
        if (logs[logIndex].flags & 1)
          QuestJournal::Apply(actor,
                              { { "quest", targetId }, { "op", "complete" } });
      }
      return true;
    };
    auto dispatchReferenceEvent = [&](uint32_t targetId, const char* name,
                                      const std::vector<VarValue>& args,
                                      bool includeAliases) {
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
        const auto data = id == questId
          ? metadata
          : espm::QuestScriptData::Read(record.rec, world.GetEspmCache());
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
          if (static_cast<IGameObject*>(target) && formId(target) == targetId)
            recipients.push_back(alias);
        }
      }
      for (const auto& recipient : recipients)
        vm.SendEventSynchronous(recipient, name, args, stack);
    };
    if (loadedReference) {
      if (!QuestJournal::ReferenceInitialized(actor, loadedReference)) {
        // An alias's OnInit belongs to quest startup, not its target's load.
        dispatchReferenceEvent(loadedReference, "OnInit", {}, false);
        QuestJournal::MarkReferenceInitialized(actor, loadedReference);
      }
      dispatchReferenceEvent(loadedReference, "OnCellLoad", {}, true);
      dispatchReferenceEvent(loadedReference, "OnLoad", {}, true);
      changed = true;
    } else {
      changed = execute(questId, stage);
      if (!changed)
        return;
    }
    for (size_t eventIndex = 0; eventIndex < inventoryEvents.size();
         ++eventIndex) {
      // Copy before dispatch: handlers may append more inventory events.
      const auto event = inventoryEvents[eventIndex];
      const auto* name = event.added ? "OnItemAdded" : "OnItemRemoved";
      const std::vector<VarValue> args{
        VarValue(resolve(event.item).get()),
        VarValue(static_cast<int32_t>(event.count)), VarValue::None(),
        event.container ? VarValue(resolve(event.container).get())
                        : VarValue::None()
      };
      dispatchReferenceEvent(event.target, name, args, true);
    }
    if (loadedReference &&
        &CampaignReferenceBinding::Resolve(actor, loadedReference) != loaded)
      throw std::runtime_error(
        "Campaign load reference changed during execution");
    for (const auto& [key, holder] : variables)
      state.SetScript(key, holder->Save());
    QuestJournal::SetScriptState(actor, state.Save());
  });
  return changed;
}
