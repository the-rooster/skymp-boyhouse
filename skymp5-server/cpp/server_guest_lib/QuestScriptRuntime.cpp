#include "QuestScriptRuntime.h"
#include "ConditionsEvaluator.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "condition_functions/ConditionFunctionFactory.h"
#include "libespm/CTDA.h"
#include "libespm/RecordHeaderAccess.h"
#include "papyrus-vm/Reader.h"
#include "script_classes/PapyrusQuest.h"
#include "script_compatibility_policies/HeuristicPolicy.h"
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

class Variables : public IVariablesHolder
{
public:
  using Resolve =
    std::function<std::shared_ptr<CampaignQuestObject>(uint32_t)>;
  Variables(espm::QuestScript script_, espm::LookupResult record_, Json saved_,
            Resolve resolve_, const std::vector<std::string>& files_)
    : script(std::move(script_))
    , record(record_)
    , saved(std::move(saved_))
    , resolve(std::move(resolve_))
    , files(files_)
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
      initial = Decode(saved.at(qualified));
    for (const auto& variable : pex.objectTable.at(0).variables)
      if (Lower(variable.name) == key)
        initial.objectType = variable.typeName;
    return &values.emplace(qualified, std::move(initial)).first->second;
  }

  Json Save(std::set<const void*>& arraysSeen) const
  {
    auto result = saved;
    for (const auto& [name, value] : values)
      result[name] = Encode(value, arraysSeen);
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
          throw std::runtime_error(
            "Quest alias property requires an alias adapter");
        return VarValue(resolve(record.ToGlobalId(value.formId)).get());
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
  Json Encode(const VarValue& value, std::set<const void*>& arraysSeen) const
  {
    if (value.promise)
      throw std::runtime_error("Cannot save latent quest value");
    Json result{ { "type", value.GetType() } };
    switch (value.GetType()) {
      case VarValue::kType_Object: {
        auto* object = static_cast<IGameObject*>(value);
        if (!object)
          result["value"] = nullptr;
        else {
          auto* quest = dynamic_cast<CampaignQuestObject*>(object);
          if (!quest)
            throw std::runtime_error("Unsupported saved quest object");
          result["value"] =
            FormDesc::FromFormId(
              quest->record.ToGlobalId(quest->record.rec->GetId()), files)
              .ToString();
        }
        break;
      }
      case VarValue::kType_String:
        result["value"] = std::string(static_cast<const char*>(value));
        break;
      case VarValue::kType_Integer:
        result["value"] = static_cast<int32_t>(value);
        break;
      case VarValue::kType_Float: {
        const auto number = static_cast<double>(value);
        if (!std::isfinite(number))
          throw std::runtime_error("Nonfinite quest variable");
        result["value"] = number;
        break;
      }
      case VarValue::kType_Bool:
        result["value"] = static_cast<bool>(value);
        break;
      default:
        if (value.GetType() < 11 || value.GetType() > 15)
          throw std::runtime_error("Unsupported quest variable type");
        result["value"] = nullptr;
        if (value.pArray) {
          if (!arraysSeen.insert(value.pArray.get()).second)
            throw std::runtime_error(
              "Shared quest arrays require reference-preserving persistence");
          result["value"] = Json::array();
          for (const auto& element : *value.pArray)
            result["value"].push_back(Encode(element, arraysSeen));
        }
    }
    return result;
  }
  VarValue Decode(const Json& json)
  {
    if (!json.at("type").is_number_integer())
      throw std::runtime_error("Invalid saved type tag");
    const auto type = json.at("type").get<int>();
    const auto& value = json.at("value");
    switch (type) {
      case 0:
        return value.is_null()
          ? VarValue::None()
          : VarValue(
              resolve(
                FormDesc::FromString(value.get<std::string>()).ToFormId(files))
                .get());
      case 2:
        return VarValue(value.get<std::string>());
      case 3: {
        const auto integer = value.get<int64_t>();
        if (!value.is_number_integer() || integer < INT32_MIN ||
            integer > INT32_MAX)
          throw std::runtime_error("Invalid saved integer");
        return VarValue(int32_t(integer));
      }
      case 4: {
        const auto number = value.get<double>();
        if (!std::isfinite(number))
          throw std::runtime_error("Invalid saved float");
        return VarValue(number);
      }
      case 5:
        return VarValue(value.get<bool>());
      default:
        if (type < 11 || type > 15)
          throw std::runtime_error("Invalid saved variable type");
        VarValue array(static_cast<uint8_t>(type));
        if (!value.is_null()) {
          if (!value.is_array() || value.size() > 1000000)
            throw std::runtime_error("Invalid saved array");
          array.pArray = std::make_shared<std::vector<VarValue>>();
          for (const auto& element : value) {
            auto decoded = Decode(element);
            if (decoded.GetType() != (type == 11 ? 0 : type - 10))
              throw std::runtime_error("Saved array element type mismatch");
            array.pArray->push_back(std::move(decoded));
          }
        }
        return array;
    }
  }
  espm::QuestScript script;
  espm::LookupResult record;
  Json saved;
  Resolve resolve;
  const std::vector<std::string>& files;
  std::map<std::string, VarValue> values;
};

struct LogEntry
{
  uint8_t flags = 0;
  std::vector<Condition> conditions;
};
std::vector<LogEntry> ReadLogs(const espm::LookupResult& record,
                               uint16_t stage, WorldState& world)
{
  bool selected = false;
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
      } else if (!std::memcmp(tag, "QOBJ", 4) || !std::memcmp(tag, "ANAM", 4))
        selected = false;
      else if (selected && !std::memcmp(tag, "QSDT", 4)) {
        if (size != 1 || (uint8_t(data[0]) & ~1))
          throw std::runtime_error("Unsupported quest log flags");
        logs.push_back({ uint8_t(data[0]), {} });
      } else if (selected && !std::memcmp(tag, "CTDA", 4)) {
        if (logs.empty() || size != sizeof(espm::CTDA))
          throw std::runtime_error("Malformed quest log condition");
        espm::CTDA ctda;
        std::memcpy(&ctda, data, sizeof(ctda));
        // Only implemented quest predicates and explicitly resolved player
        // scope.
        if ((ctda.operatorFlag & 0x1e) ||
            static_cast<int>(ctda.GetOperator()) > 5 ||
            (ctda.functionIndex != 56 && ctda.functionIndex != 58 &&
             ctda.functionIndex != 59 && ctda.functionIndex != 543) ||
            (ctda.runOnType != espm::CTDA::RunOnTypeFlags::Subject &&
             !(ctda.runOnType == espm::CTDA::RunOnTypeFlags::Reference &&
               record.ToGlobalId(ctda.reference) == 0x14)))
          throw std::runtime_error(
            "Unsupported quest log condition or run-on context");
        auto condition = Condition::FromCtda(ctda);
        condition.runsOn = "Subject";
        condition.parameter1 = fmt::format(
          "0x{:x}", record.ToGlobalId(ctda.GetDefaultData().firstParameter));
        logs.back().conditions.push_back(std::move(condition));
      }
    },
    world.GetEspmCache());
  return logs;
}

}

bool QuestScriptRuntime::ExecuteStage(MpActor& actor, uint32_t questId,
                                      uint16_t stage)
{
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  auto metadata =
    espm::QuestScriptData::Read(record.rec, world.GetEspmCache());
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
  return ExecuteStage(actor, questId, stage, metadata, scripts);
}

bool QuestScriptRuntime::ExecuteStage(
  MpActor& actor, uint32_t questId, uint16_t stage,
  const espm::QuestScriptData& metadata,
  const std::vector<PexScript::Lazy>& scripts)
{
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  if (!record.rec || record.rec->GetType() != "QUST")
    throw std::invalid_argument("Expected quest record");
  const auto logs = ReadLogs(record, stage, world);
  bool changed = false;
  QuestJournal::Atomic(actor, [&] {
    changed = QuestJournal::Apply(
      actor,
      { { "quest", questId }, { "op", "setStage" }, { "index", stage } });
    if (!changed)
      return;
    auto state = QuestJournal::ScriptState(actor);
    VirtualMachine vm(scripts);
    vm.SetStrictExecution(true);
    auto policy = std::make_shared<HeuristicPolicy>(&world);
    PapyrusQuest questApi;
    questApi.Register(vm, policy);
    for (const auto* name :
         { "Start", "Stop", "Reset", "SetStage", "SetCurrentStageID" }) {
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
    std::map<uint32_t, std::shared_ptr<CampaignQuestObject>> objects;
    std::map<std::string, std::shared_ptr<Variables>> variables;
    std::function<std::shared_ptr<CampaignQuestObject>(uint32_t)> resolve;
    resolve = [&](uint32_t id) {
      if (objects.count(id))
        return objects.at(id);
      const auto rec = world.GetEspm().GetBrowser().LookupById(id);
      if (!rec.rec || rec.rec->GetType() != "QUST")
        throw std::runtime_error(
          "World reference properties require a campaign overlay adapter");
      auto object = std::make_shared<CampaignQuestObject>(rec, owner);
      objects[id] = object;
      const auto data = id == questId
        ? metadata
        : espm::QuestScriptData::Read(rec.rec, world.GetEspmCache());
      std::vector<VirtualMachine::ScriptInfo> bindings;
      for (const auto& script : data.scripts) {
        if (script.status == 3)
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
          script, rec, state.value(key, Json::object()), resolve,
          world.espmFiles);
        variables[key] = holder;
        bindings.push_back({ script.name, holder });
      }
      vm.AddObject(object, bindings);
      return object;
    };
    auto object = resolve(questId);
    const auto conditionFunctions =
      ConditionFunctionFactory::CreateConditionFunctions();
    std::set<uint32_t> seen;
    for (const auto& fragment : metadata.fragments) {
      if (fragment.stage != stage)
        continue;
      if (fragment.logEntry >= logs.size() ||
          !seen.insert(fragment.logEntry).second)
        throw std::runtime_error(
          "Invalid or duplicate quest fragment log binding");
    }
    for (size_t logIndex = 0; logIndex < logs.size(); ++logIndex) {
      bool passes = logs[logIndex].conditions.empty();
      if (!passes)
        ConditionsEvaluator::EvaluateConditions(
          conditionFunctions, {}, ConditionsEvaluatorCaller::kCraft,
          logs[logIndex].conditions, actor, actor,
          [&](bool result, auto&) { passes = result; });
      if (!passes)
        continue;
      for (const auto& fragment : metadata.fragments) {
        if (fragment.stage != stage || fragment.logEntry != logIndex)
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
          throw std::runtime_error(fragment.script + "." + fragment.function +
                                   ": " + error.what());
        }
        if (result.promise)
          throw std::runtime_error(
            "Latent quest fragment requires durable continuation support");
      }
      if (logs[logIndex].flags & 1)
        QuestJournal::Apply(actor,
                            { { "quest", questId }, { "op", "complete" } });
    }
    std::set<const void*> arraysSeen;
    for (const auto& [key, holder] : variables)
      state[key] = holder->Save(arraysSeen);
    QuestJournal::SetScriptState(actor, state);
  });
  return changed;
}
