#include "papyrus-vm/VirtualMachine.h"
#include "papyrus-vm/Utils.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace {
constexpr uint32_t g_maxStackId = 100'000;
}

void VirtualMachine::CheckExecutionBudget()
{
  if (strictExecution && ++executedInstructions > 100000)
    throw std::runtime_error("Papyrus transaction instruction quota exceeded");
}

VirtualMachine::VirtualMachine(
  const std::vector<PexScript::Lazy>& loadedScripts)
{
  stackIdMaker.reset(new MakeID(g_maxStackId));

  for (auto& script : loadedScripts) {
    allLoadedScripts[CIString{ script.source.begin(), script.source.end() }] =
      script;
  }
}

VirtualMachine::VirtualMachine(
  const std::vector<std::shared_ptr<PexScript>>& loadedScripts)
{
  stackIdMaker.reset(new MakeID(g_maxStackId));

  for (auto& script : loadedScripts) {
    allLoadedScripts[CIString{ script->source.begin(),
                               script->source.end() }] = {
      script->source, [script] { return script; }
    };
  }
}

void VirtualMachine::SetMissingScriptHandler(
  const MissingScriptHandler& handler)
{
  this->missingScriptHandler = handler;
}

std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), tolower);
  return s;
}

void VirtualMachine::RegisterFunction(const std::string& className,
                                      const std::string& functionName,
                                      const FunctionType& type,
                                      const NativeFunction& fn)
{
  switch (type) {
    case FunctionType::GlobalFunction:

      nativeStaticFunctions[className.data()][functionName.data()] = fn;
      break;
    case FunctionType::Method:
      nativeFunctions[className.data()][functionName.data()] = fn;
      break;
  }
}

std::set<CIString> VirtualMachine::ListClasses() const
{
  std::set<CIString> result;

  for (auto& f : nativeStaticFunctions) {
    result.insert(f.first.data());
  }

  for (auto& f : nativeFunctions) {
    result.insert(f.first.data());
  }

  return result;
}

CIString VirtualMachine::GetBaseClass(const CIString& className) const
{
  auto it = allLoadedScripts.find(className);
  if (it != allLoadedScripts.end()) {
    return it->second.fn()->objectTable[0].parentClassName.data();
  }
  return CIString();
}

std::set<CIString> VirtualMachine::ListStaticFunctions(
  const CIString& className) const
{
  std::set<CIString> result;

  auto it = nativeStaticFunctions.find(className.data());
  if (it != nativeStaticFunctions.end()) {
    for (auto& f : it->second) {
      result.insert(f.first.data());
    }
  }

  return result;
}

std::set<CIString> VirtualMachine::ListMethods(const CIString& className) const
{
  std::set<CIString> result;

  auto it = nativeFunctions.find(className.data());
  if (it != nativeFunctions.end()) {
    for (auto& f : it->second) {
      result.insert(f.first.data());
    }
  }

  return result;
}

NativeFunction VirtualMachine::GetFunctionImplementation(
  const CIString& className, const CIString& functionName, bool isStatic) const
{

  if (!isStatic) {
    auto it = nativeFunctions.find(className.data());
    if (it != nativeFunctions.end()) {
      auto it2 = it->second.find(functionName.data());
      if (it2 != it->second.end()) {
        return it2->second;
      }
    }
  }

  if (isStatic) {
    auto it3 = nativeStaticFunctions.find(className.data());
    if (it3 != nativeStaticFunctions.end()) {
      auto it4 = it3->second.find(functionName.data());
      if (it4 != it3->second.end()) {
        return it4->second;
      }
    }
  }

  return nullptr;
}

bool VirtualMachine::DynamicCast(const VarValue& object,
                                 const CIString& className) const
{
  VarValue scriptToCastOwner = object;
  VarValue result;
  result.objectType = className.data();
  ActivePexInstance::CastObjectToObject(*this, &result, &scriptToCastOwner);

  if (static_cast<IGameObject*>(result) != nullptr) {
    return true;
  }

  return false;
}

void VirtualMachine::AddObject(std::shared_ptr<IGameObject> self,
                               const std::vector<ScriptInfo>& scripts)
{
  std::vector<std::shared_ptr<ActivePexInstance>> scriptsForObject;

  for (auto& s : scripts) {
    CIString ciNameNeedScript{ s.name.begin(), s.name.end() };
    auto it = allLoadedScripts.find(ciNameNeedScript);
    if (it != allLoadedScripts.end()) {
      auto scriptInstance = std::make_shared<ActivePexInstance>(
        it->second, s.vars, this, VarValue((IGameObject*)self.get()), "");
      scriptsForObject.push_back(scriptInstance);
    }
  }

  for (auto& script : scriptsForObject) {
    self->AddScript(script);
  }
  gameObjectsHolder.insert(self);
}

void VirtualMachine::SendEvent(std::shared_ptr<IGameObject> self,
                               const char* eventName,
                               const std::vector<VarValue>& arguments,
                               OnEnter enter)
{
  for (auto& scriptInstance : self->ListActivePexInstances()) {
    auto name = scriptInstance->GetActiveStateName();

    // TODO: const&
    auto fn = scriptInstance->GetFunctionByName(
      eventName, scriptInstance->GetActiveStateName());
    if (fn.valid) {
      std::shared_ptr<StackData> stackData;
      stackData.reset(new StackData{ StackIdHolder{ *this } });
      if (enter) {
        enter(*stackData);
      }
      scriptInstance->StartFunction(
        fn, const_cast<std::vector<VarValue>&>(arguments), stackData);
    }
  }
}

void VirtualMachine::SendEvent(ActivePexInstance* instance,
                               const char* eventName,
                               const std::vector<VarValue>& arguments)
{
  // TODO: const&
  auto fn =
    instance->GetFunctionByName(eventName, instance->GetActiveStateName());
  if (fn.valid) {
    std::shared_ptr<StackData> stackData;
    stackData.reset(new StackData{ StackIdHolder{ *this } });
    instance->StartFunction(fn, const_cast<std::vector<VarValue>&>(arguments),
                            stackData);
  }
}

StackIdHolder::StackIdHolder(VirtualMachine& vm_)
  : makeId(vm_.stackIdMaker)
{
  if (!makeId->CreateID(*reinterpret_cast<uint32_t*>(&stackId))) {
    throw std::runtime_error("CreateID failed to create id for stack");
  }
}

StackIdHolder::~StackIdHolder()
{
  bool destroyed = makeId->DestroyID(stackId);
  (void)destroyed;
  assert(destroyed);
}

int32_t StackIdHolder::GetStackId() const
{
  return stackId;
}

StackDepthHolder::StackDepthHolder() = default;

size_t StackDepthHolder::GetStackDepth() const
{
  return depth;
}

void StackDepthHolder::IncreaseStackDepth()
{
  ++depth;
}

void StackDepthHolder::DecreaseStackDepth()
{
  --depth;
}

VarValue VirtualMachine::CallMethod(
  IGameObject* selfObj, const char* methodName,
  std::vector<VarValue>& arguments, std::shared_ptr<StackData> stackData,
  const std::vector<std::shared_ptr<ActivePexInstance>>*
    activePexInstancesOverride,
  const char* receiverType)
{
  if (!stackData) {
    stackData.reset(new StackData{ StackIdHolder{ *this } });
  }

  if (!selfObj) {
    if (strictExecution)
      throw std::runtime_error("Method called on None");
    return VarValue::None();
  }

  const auto& instances = activePexInstancesOverride
    ? *activePexInstancesOverride
    : selfObj->ListActivePexInstances();

  std::string declaredNative;
  if (strictExecution) {
    const bool stateFunction = !Utils::stricmp(methodName, "GotoState") ||
      !Utils::stricmp(methodName, "GetState");
    const bool stateEvent = !Utils::stricmp(methodName, "OnBeginState") ||
      !Utils::stricmp(methodName, "OnEndState");
    std::shared_ptr<ActivePexInstance> selected;
    FunctionInfo function;
    bool sharedEngineHelper = false;
    bool matchingType = !receiverType || !receiverType[0];
    for (const auto& root : instances) {
      if (!root || !root->IsValid())
        throw std::runtime_error("Invalid Papyrus method script instance");
      bool matches = !receiverType || !receiverType[0];
      for (auto entry = root; entry && entry->IsValid();
           entry = entry->GetParentInstance())
        matches |= receiverType &&
          !Utils::stricmp(entry->GetSourcePexName().c_str(), receiverType);
      if (!matches)
        continue;
      matchingType = true;
      const auto state = stateFunction ? "" : root->GetActiveStateName();
      for (auto entry = root; entry && entry->IsValid();
           entry = entry->GetParentInstance()) {
        auto candidate = entry->GetFunctionByName(methodName, state);
        if (!candidate.valid && !state.empty())
          candidate = entry->GetFunctionByName(methodName, "");
        if (!candidate.valid)
          continue;
        if (selected) {
          const bool sameDeclaration =
            !Utils::stricmp(selected->GetSourcePexName().c_str(),
                            entry->GetSourcePexName().c_str());
          // Several attachments can inherit the same native declaration or
          // engine wrapper (e.g. Quest.SetStage). Use its registered campaign
          // implementation once; never choose an arbitrary attachment for a
          // script body that could read private variables or dispatch self.
          if (sameDeclaration &&
              nativeFunctions[entry->GetSourcePexName().c_str()][methodName])
            sharedEngineHelper = true;
          else if (!(sameDeclaration && function.IsNative() &&
                     candidate.IsNative()))
            throw std::runtime_error(
              std::string("Ambiguous Papyrus method binding: ") + methodName);
        }
        selected = entry;
        function = candidate;
        break;
      }
    }
    if (!matchingType) {
      // Native objects can have no attached scripts. A custom receiver must
      // resolve to a binding; it must never fall through to another script.
      std::string base = selfObj->GetParentNativeScript();
      std::set<CIString> visited;
      while (!base.empty()) {
        if (!visited.insert(CIString{ base.begin(), base.end() }).second ||
            visited.size() > 128)
          throw std::runtime_error(
            "Papyrus native class cycle/depth exceeded");
        if (!Utils::stricmp(base.c_str(), receiverType)) {
          matchingType = true;
          break;
        }
        const auto pex = GetPexByName(base);
        if (!pex.fn)
          break;
        base = pex.fn()->objectTable.at(0).parentClassName;
      }
      if (!matchingType)
        throw std::runtime_error(
          "Papyrus receiver has no matching script binding");
    }
    if (selected) {
      if (function.IsGlobal())
        throw std::runtime_error("Global Papyrus function called as a method");
      if (function.IsNative() || sharedEngineHelper) {
        if (arguments.size() != function.params.size())
          throw std::runtime_error("Papyrus native argument count mismatch");
        declaredNative = selected->GetSourcePexName();
      } else
        return selected->StartFunction(function, arguments, stackData);
    } else if (stateEvent) {
      if (!arguments.empty())
        throw std::runtime_error(
          "Papyrus state event argument count mismatch");
      return VarValue::None(); // The implicit state events have empty
                               // defaults.
    }
  } else
    for (auto& activeScript : instances) {
      // TODO: const& (ref/optional)
      FunctionInfo functionInfo;

      if (!Utils::stricmp(methodName, "GotoState") ||
          !Utils::stricmp(methodName, "GetState")) {
        functionInfo = activeScript->GetFunctionByName(methodName, "");
      } else {
        functionInfo = activeScript->GetFunctionByName(
          methodName, activeScript->GetActiveStateName());
        if (!functionInfo.valid)
          functionInfo = activeScript->GetFunctionByName(methodName, "");
      }

      if (functionInfo.valid) {
        return activeScript->StartFunction(functionInfo, arguments, stackData);
      }
    }

  // natives have to be after non-natives (Bethesda overrides native functions
  // in some scripts)
  const char* nativeClass = declaredNative.empty()
    ? selfObj->GetParentNativeScript()
    : declaredNative.c_str();
  const char* base = nativeClass;
  std::set<CIString> nativeVisited;
  while (1) {
    if (strictExecution &&
        (!nativeVisited.insert(CIString{ base, base + std::strlen(base) })
            .second ||
         nativeVisited.size() > 128))
      throw std::runtime_error("Papyrus native class cycle/depth exceeded");
    if (auto f = nativeFunctions[base][methodName]) {
      auto self = VarValue(selfObj);
      self.SetMetaStackIdHolder(stackData->stackIdHolder);
      try {
        return f(self, arguments);
      } catch (std::exception& e) {
        if (strictExecution)
          throw;
        std::string methodNameFull;
        methodNameFull += base;
        methodNameFull += (base[0] ? "." : "") + std::string(methodName) + "'";
        spdlog::error("VirtualMachine::CallMethod - Native {} errored with {}",
                      methodNameFull, e.what());
        return VarValue::None();
      } catch (...) {
        if (strictExecution)
          throw;
        std::string methodNameFull;
        methodNameFull += base;
        methodNameFull += (base[0] ? "." : "") + std::string(methodName) + "'";
        spdlog::critical(
          "VirtualMachine::CallMethod - Native {} errored with unknown error",
          methodNameFull);
        std::terminate();
        return VarValue::None();
      }
    }
    if (strictExecution && !declaredNative.empty())
      throw std::runtime_error("Unimplemented native: " + declaredNative +
                               "." + methodName);
    auto it = allLoadedScripts.find(base);
    if (it == allLoadedScripts.end())
      break;
    base = it->second.fn()->objectTable[0].parentClassName.data();
    if (!base[0])
      break;
  }

  std::string e = "Method not found - '";
  e += base;
  e += (base[0] ? "." : "") + std::string(methodName) + "'";

  if (strictExecution)
    throw std::runtime_error(e);
  spdlog::error("VirtualMachine::CallMethod - {}", e);

  return VarValue::None();
}

VarValue VirtualMachine::CallStatic(const std::string& className,
                                    const std::string& functionName,
                                    std::vector<VarValue>& arguments,
                                    std::shared_ptr<StackData> stackData)
{
  if (!stackData) {
    stackData.reset(new StackData{ StackIdHolder{ *this } });
  }

  VarValue result = VarValue::None();

  auto f = nativeStaticFunctions[className.data()][functionName.data()]
    ? nativeStaticFunctions[className.data()][functionName.data()]
    : nativeStaticFunctions[""][functionName.data()];

  if (f) {
    auto self = VarValue::None();
    self.SetMetaStackIdHolder(stackData->stackIdHolder);
    try {
      return f(self, arguments);
    } catch (std::exception& e) {
      if (strictExecution)
        throw;
      std::string functionNameFull = className + "." + functionName;
      spdlog::error("VirtualMachine::CallStatic - Native {} errored with {}",
                    functionNameFull, e.what());
      return VarValue::None();
    } catch (...) {
      if (strictExecution)
        throw;
      std::string functionNameFull = className + "." + functionName;
      spdlog::critical(
        "VirtualMachine::CallStatic - Native {} errored with unknown error",
        functionNameFull);
      std::terminate();
      return VarValue::None();
    }
  }

  auto classNameCi = CIString{ className.begin(), className.end() };
  auto it = allLoadedScripts.find(classNameCi);
  if (it == allLoadedScripts.end()) {
    if (this->missingScriptHandler) {
      if (auto newScript = this->missingScriptHandler(className)) {
        allLoadedScripts[classNameCi] = *newScript;
      }
    }
    it = allLoadedScripts.find(classNameCi);
    if (it == allLoadedScripts.end())
      throw std::runtime_error("script is missing - '" + className + "'");
  }

  auto& instance = instancesForStaticCalls[className];
  if (!instance) {
    instance = std::make_shared<ActivePexInstance>(it->second, nullptr, this,
                                                   VarValue::None(), "");
  }

  // TODO: const&
  FunctionInfo function;
  function = instance->GetFunctionByName(functionName.c_str(), "");

  if (function.valid) {
    if (function.IsNative()) {
      if (strictExecution)
        throw std::runtime_error("Unimplemented native: " + className + "." +
                                 functionName);
      spdlog::error("VirtualMachine::CallStatic - Function not found - '{}'",
                    functionName);
      return VarValue::None();
    }

    result = instance->StartFunction(function, arguments, stackData);
  }
  if (!function.valid) {
    if (strictExecution)
      throw std::runtime_error("Missing static function: " + className + "." +
                               functionName);
    spdlog::error("VirtualMachine::CallStatic - Function is not valid - '{}'",
                  functionName);
    return VarValue::None();
  }

  return result;
}

PexScript::Lazy VirtualMachine::GetPexByName(const std::string& name) const
{
  auto it = allLoadedScripts.find(CIString{ name.begin(), name.end() });
  if (it != allLoadedScripts.end())
    return it->second;
  return PexScript::Lazy();
}

std::shared_ptr<ActivePexInstance> VirtualMachine::CreateActivePexInstance(
  const std::string& pexScriptName, VarValue activeInstanceOwner,
  const std::shared_ptr<IVariablesHolder>& mapForFillProperties,
  const std::string& childrenName)
{

  auto it = allLoadedScripts.find(
    CIString{ pexScriptName.begin(), pexScriptName.end() });
  if (it != allLoadedScripts.end()) {
    if (strictExecution) {
      std::set<CIString> visited;
      auto parent = pexScriptName;
      while (!parent.empty()) {
        if (!visited.insert(CIString{ parent.begin(), parent.end() }).second ||
            visited.size() > 128)
          throw std::runtime_error("Papyrus inheritance cycle/depth exceeded");
        const auto pex = GetPexByName(parent);
        if (!pex.fn)
          throw std::runtime_error("Unable to find parent script '" + parent +
                                   "'");
        parent = pex.fn()->objectTable.at(0).parentClassName;
      }
    }
    ActivePexInstance scriptInstance(it->second, mapForFillProperties, this,
                                     activeInstanceOwner, childrenName);
    return std::make_shared<ActivePexInstance>(scriptInstance);
  }

  static const std::shared_ptr<ActivePexInstance> notValidInstance =
    std::make_shared<ActivePexInstance>();

  if (pexScriptName != "")
    throw std::runtime_error("Unable to find script '" + pexScriptName + "'");

  return notValidInstance;
}

bool VirtualMachine::IsNativeFunctionByNameExisted(
  const std::string& name) const
{
  for (auto& staticFunction : nativeStaticFunctions) {
    if (staticFunction.first == name.data())
      return true;
  }

  for (auto& metod : nativeFunctions) {
    for (auto& func : metod.second)
      if (func.first == name.data())
        return true;
  }

  return false;
}

void VirtualMachine::RemoveObject(std::shared_ptr<IGameObject> self)
{
}
