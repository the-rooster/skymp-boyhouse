#include "QuestScriptRuntime.h"
#include "ConditionsEvaluator.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
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

class Variables : public IVariablesHolder
{
public:
  using Resolve = std::function<std::shared_ptr<IGameObject>(uint32_t)>;
  Variables(espm::QuestScript script_, espm::LookupResult record_, Json saved_,
            Resolve resolve_, QuestVmState& heap_)
    : script(std::move(script_))
    , record(record_)
    , saved(std::move(saved_))
    , resolve(std::move(resolve_))
    , heap(heap_)
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
  espm::QuestScript script;
  espm::LookupResult record;
  Json saved;
  Resolve resolve;
  QuestVmState& heap;
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
             ctda.functionIndex != 59 && ctda.functionIndex != 543 &&
             ctda.functionIndex != 74) ||
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
  bool changed = false;
  QuestJournal::Atomic(actor, [&] {
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
    QuestVmState state(
      QuestJournal::ScriptState(actor),
      [&](IGameObject* value) -> Json {
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
        return VarValue(resolve(FormDesc::FromString(value.get<std::string>())
                                  .ToFormId(world.espmFiles))
                          .get());
      });

    resolve = [&](uint32_t id) -> std::shared_ptr<IGameObject> {
      if (objects.count(id))
        return objects.at(id);
      const auto rec = world.GetEspm().GetBrowser().LookupById(id);
      if (rec.rec && rec.rec->GetType() == "GLOB") {
        auto global = std::make_shared<CampaignGlobalObject>(rec, owner);
        objects[id] = global;
        return global;
      }

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
          script, rec, state.GetScript(key), resolve, state);
        variables[key] = holder;
        bindings.push_back({ script.name, holder });
      }
      vm.AddObject(object, bindings);
      return object;
    };

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
    auto conditionFunctions =
      ConditionFunctionFactory::CreateConditionFunctions();
    conditionFunctions.RegisterConditionFunction(
      std::make_shared<CampaignGlobalCondition>([&](uint32_t id) {
        const auto value =
          std::dynamic_pointer_cast<CampaignGlobalObject>(resolve(id));
        if (!value)
          throw std::runtime_error("Condition expected a campaign global");
        return readGlobal(*value);
      }));
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
        bool passes = logs[logIndex].conditions.empty();
        if (!passes)
          ConditionsEvaluator::EvaluateConditions(
            conditionFunctions, {}, ConditionsEvaluatorCaller::kCraft,
            logs[logIndex].conditions, actor, actor,
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
    changed = execute(questId, stage);
    if (!changed)
      return;
    for (const auto& [key, holder] : variables)
      state.SetScript(key, holder->Save());
    QuestJournal::SetScriptState(actor, state.Save());
  });
  return changed;
}
