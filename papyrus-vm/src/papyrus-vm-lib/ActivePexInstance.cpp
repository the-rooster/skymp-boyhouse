#include "papyrus-vm/FunctionInfo.h"
#include "papyrus-vm/OpcodesImplementation.h"
#include "papyrus-vm/Utils.h"
#include "papyrus-vm/VirtualMachine.h"

#include "ScopedTask.h"

#include <algorithm>
#include <cctype> // tolower
#include <cmath>
#include <fmt/ranges.h>
#include <functional>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace {
bool IsSelfStr(const VarValue& v)
{
  return v.GetType() == VarValue::kType_String &&
    !Utils::stricmp("self", static_cast<const char*>(v));
}
}

ActivePexInstance::ActivePexInstance()
{
  this->parentVM = nullptr;
}

ActivePexInstance::ActivePexInstance(
  PexScript::Lazy sourcePex,
  const std::shared_ptr<IVariablesHolder>& mapForFillProperties,
  VirtualMachine* parentVM, VarValue activeInstanceOwner,
  std::string childrenName)
{
  this->childrenName = childrenName;
  this->activeInstanceOwner = activeInstanceOwner;
  this->parentVM = parentVM;
  this->sourcePex = sourcePex;
  this->parentInstance =
    FillParentInstance(sourcePex.fn()->objectTable[0].parentClassName,
                       activeInstanceOwner, mapForFillProperties);

  this->variables = mapForFillProperties;

  this->_IsValid = true;
}

std::shared_ptr<ActivePexInstance> ActivePexInstance::FillParentInstance(
  std::string nameNeedScript, VarValue activeInstanceOwner,
  const std::shared_ptr<IVariablesHolder>& mapForFillProperties)
{
  return parentVM->CreateActivePexInstance(nameNeedScript, activeInstanceOwner,
                                           mapForFillProperties,
                                           this->sourcePex.source);
}

FunctionInfo ActivePexInstance::GetFunctionByName(const char* name,
                                                  std::string stateName) const
{
  // static FunctionInfo kInvalidFunction;
  FunctionInfo function;
  for (auto& object : sourcePex.fn()->objectTable) {
    for (auto& state : object.states) {
      if (state.name == stateName ||
          (parentVM->IsStrictExecution() &&
           !Utils::stricmp(state.name.c_str(), stateName.c_str()))) {
        for (auto& func : state.functions) {
          if (!Utils::stricmp(func.name.data(), name)) {
            // return func; ???
            function = func.function; // ???
            function.valid = true;
            return function;
          }
        }
      }
    }
  }
  return function;
}

std::string ActivePexInstance::GetActiveStateName() const
{
  VarValue* var = nullptr;

  try {
    const auto* instance = this;
    if (parentVM->IsStrictExecution())
      instance = GetAttachedRoot().get();
    if (!instance->variables)
      throw std::runtime_error("Missing Papyrus script variables");
    var = instance->variables->GetVariableByName("::State",
                                                 *instance->sourcePex.fn());
  } catch (std::exception& e) {
    if (parentVM->IsStrictExecution())
      throw;
    spdlog::error("ActivePexInstance::GetActiveStateName - "
                  "GetVariableByName(::State) unexpectedly errored: '{}'",
                  e.what());
    return "";
  } catch (...) {
    if (parentVM->IsStrictExecution())
      throw;
    spdlog::critical(
      "ActivePexInstance::GetActiveStateName - GetVariableByName(::State) "
      "unexpectedly errored: unknown error");
    std::terminate();
    return "";
  }

  if (parentVM->IsStrictExecution() &&
      (!var || var->GetType() != VarValue::kType_String ||
       !static_cast<const char*>(*var)))
    throw std::runtime_error("Invalid Papyrus instance state");
  if (!var) {
    spdlog::error("ActivePexInstance::GetActiveStateName - ::State variable "
                  "doesn't exist in ActivePexInstance");
    return "";
  }

  return static_cast<const char*>(*var);
}

Object::PropInfo* ActivePexInstance::GetProperty(
  const ActivePexInstance& scriptInstance, std::string propertyName,
  uint8_t flag)
{
  if (!scriptInstance.IsValid())
    return nullptr;

  if (flag == Object::PropInfo::kFlags_Read) {

    for (auto& object : scriptInstance.sourcePex.fn()->objectTable) {
      for (auto& prop : object.properties) {
        if (prop.name == propertyName &&
            (prop.flags & 5) == prop.kFlags_Read) {
          return &prop;
        }
      }
    }

    if (flag == Object::PropInfo::kFlags_Write) {

      for (auto& object : scriptInstance.sourcePex.fn()->objectTable) {
        for (auto& prop : object.properties) {
          if (prop.name == propertyName &&
              (prop.flags & 6) == prop.kFlags_Write) {
            return &prop;
          }
        }
      }
    }
  }

  return nullptr;
}

const std::string& ActivePexInstance::GetSourcePexName() const
{
  if (!sourcePex.fn()) {
    static const std::string kEmptyName = "";
    return kEmptyName;
  }

  return sourcePex.fn()->source;
}

struct ActivePexInstance::ExecutionContext
{
  std::shared_ptr<StackData> stackData;
  std::vector<FunctionCode::Instruction> instructions;
  std::shared_ptr<std::vector<Local>> locals;
  bool needReturn = false;
  bool needJump = false;
  int jumpStep = 0;
  VarValue returnValue = VarValue::None();
  size_t line = 0;
  PapyrusFunctionIdentity identity;
  std::string returnType;
};

std::vector<VarValue> GetArgsForCall(uint8_t op,
                                     const std::vector<VarValue*>& opcodeArgs)
{
  std::vector<VarValue> argsForCall;
  if (opcodeArgs.size() > 4) {
    for (size_t i = 4; i < opcodeArgs.size(); ++i) {
      argsForCall.push_back(*opcodeArgs[i]);
    }
  }
  if (op == FunctionCode::kOp_CallParent && opcodeArgs.size() > 3) {
    for (size_t i = 3; i < opcodeArgs.size(); ++i) {
      argsForCall.push_back(*opcodeArgs[i]);
    }
  }
  return argsForCall;
}

bool ActivePexInstance::EnsureCallResultIsSynchronous(
  const VarValue& callResult, ExecutionContext* ctx)
{
  if (parentVM->IsStrictExecution() && callResult.promise)
    throw std::runtime_error(
      "Latent call requires a durable continuation adapter");
  if (!callResult.promise) {
    return true;
  }

  Viet::Promise<VarValue> currentFnPr;

  auto ctxCopy = *ctx;
  callResult.promise->Then([this, ctxCopy, currentFnPr](VarValue v) {
    auto ctxCopy_ = ctxCopy;
    ctxCopy_.line++;
    auto res = ExecuteAll(ctxCopy_, v);

    if (res.promise)
      res.promise->Then(currentFnPr);
    else
      currentFnPr.Resolve(res);
  });

  ctx->needReturn = true;
  ctx->returnValue = VarValue(currentFnPr);
  return false;
}

std::shared_ptr<ActivePexInstance> ActivePexInstance::GetAttachedRoot() const
{
  auto* object = static_cast<IGameObject*>(activeInstanceOwner);
  if (object)
    for (const auto& root : object->ListActivePexInstances())
      for (auto instance = root; instance && instance->IsValid();
           instance = instance->GetParentInstance())
        if (instance.get() == this)
          return root;
  throw std::runtime_error("Papyrus method has no attached script instance");
}

void ActivePexInstance::ExecuteStrictProperty(
  ExecutionContext* ctx, uint8_t op, const std::vector<VarValue*>& args)
{
  const bool write = op == OpcodesImplementation::Opcodes::op_PropSet;
  const bool self =
    args.at(1) == &activeInstanceOwner || IsSelfStr(*args.at(1));
  const auto& target = self ? activeInstanceOwner : *args.at(1);
  if (target.GetType() != VarValue::kType_Object)
    throw std::runtime_error("Invalid Papyrus property target type");
  auto* object = static_cast<IGameObject*>(target);
  const char* name = static_cast<const char*>(*args.at(0));
  if (!object || !name)
    throw std::runtime_error("Invalid Papyrus property target or name");
  ActivePexInstance* selected = nullptr;
  Object::PropInfo* property = nullptr;
  std::vector<ActivePexInstance*> roots;
  if (self)
    roots.push_back(this);
  else
    for (const auto& attached : object->ListActivePexInstances())
      roots.push_back(attached.get());
  for (auto* attached : roots) {
    bool matchesType = target.objectType.empty();
    for (auto* instance = attached; instance && instance->IsValid();
         instance = instance->parentInstance.get())
      matchesType |= !Utils::stricmp(instance->GetSourcePexName().c_str(),
                                     target.objectType.c_str());
    if (!self && !matchesType)
      continue;
    for (auto* instance = attached; instance && instance->IsValid();
         instance = instance->parentInstance.get()) {
      auto& properties =
        instance->sourcePex.fn()->objectTable.at(0).properties;
      auto found =
        std::find_if(properties.begin(), properties.end(), [&](const auto& p) {
          return !Utils::stricmp(p.name.c_str(), name);
        });
      if (found == properties.end())
        continue;
      if (selected)
        throw std::runtime_error("Ambiguous Papyrus property binding");
      selected = instance;
      property = &*found;
      break;
    }
  }
  const auto access =
    write ? Object::PropInfo::kFlags_Write : Object::PropInfo::kFlags_Read;
  if (!self && !selected && parentVM->unboundMemberObserver)
    parentVM->unboundMemberObserver(
      *object, target.objectType.c_str(), name,
      write ? VirtualMachine::UnboundMember::PropertyWrite
            : VirtualMachine::UnboundMember::PropertyRead,
      write ? 1 : 0);
  if (!selected || !(property->flags & access))
    throw std::runtime_error(std::string("Unavailable Papyrus property: ") +
                             name);
  if (property->flags & Object::PropInfo::kFlags_AutoVar) {
    auto* value = selected->variables->GetVariableByName(
      property->autoVarName.c_str(), *selected->sourcePex.fn());
    if (!value)
      throw std::runtime_error("Missing Papyrus property variable");
    if (write)
      *value = *args.at(2);
    else
      *args.at(2) = *value;
  } else {
    auto function = write ? property->writeHandler : property->readHandler;
    // Like GetFunctionByName, a successful property lookup establishes
    // validity. Reader does not set the lookup-only flag on PEX handlers.
    function.valid = true;
    if (function.IsNative())
      throw std::runtime_error("Native property handler requires an adapter");
    std::vector<VarValue> arguments;
    if (write)
      arguments.push_back(*args.at(2));
    auto result = selected->StartFunction(
      function, arguments, ctx->stackData,
      { write ? "propertySet" : "propertyGet", "", property->name });
    EnsureCallResultIsSynchronous(result, ctx);
    if (!write)
      *args.at(2) = result;
  }
}

void ActivePexInstance::ExecuteOpCode(ExecutionContext* ctx, uint8_t op,
                                      const std::vector<VarValue*>& args)
{
  if (parentVM->IsStrictExecution() &&
      (op == OpcodesImplementation::Opcodes::op_PropGet ||
       op == OpcodesImplementation::Opcodes::op_PropSet)) {
    ExecuteStrictProperty(ctx, op, args);
    return;
  }
  auto argsForCall = GetArgsForCall(op, args);

  if (parentVM->IsStrictExecution()) {
    if (op == OpcodesImplementation::Opcodes::op_Array_GetElement ||
        op == OpcodesImplementation::Opcodes::op_Array_SetElement) {
      const bool get =
        op == OpcodesImplementation::Opcodes::op_Array_GetElement;
      const auto& array = *args.at(get ? 1 : 0);
      const auto& index = *args.at(get ? 2 : 1);
      if (!array.pArray || index.GetType() != VarValue::kType_Integer ||
          static_cast<int32_t>(index) < 0 ||
          static_cast<uint32_t>(static_cast<int32_t>(index)) >=
            array.pArray->size())
        throw std::runtime_error("Invalid Papyrus array access");
      if (array.GetType() < VarValue::kType_ObjectArray ||
          array.GetType() > VarValue::kType_BoolArray ||
          (!get &&
           args.at(2)->GetType() != GetArrayElementType(array.GetType())))
        throw std::runtime_error("Invalid Papyrus array element type");
    }
    if (op == OpcodesImplementation::Opcodes::op_iDiv ||
        op == OpcodesImplementation::Opcodes::op_iMod) {
      const auto denominator = static_cast<int32_t>(*args.at(2));
      if (!denominator ||
          (denominator == -1 &&
           static_cast<int32_t>(*args.at(1)) == INT32_MIN))
        throw std::runtime_error("Invalid Papyrus integer division");
    }
  }

  switch (op) {
    case OpcodesImplementation::Opcodes::op_Nop:
      break;
    case OpcodesImplementation::Opcodes::op_iAdd:
    case OpcodesImplementation::Opcodes::op_fAdd:
      *args[0] = *args[1] + (*args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_iSub:
    case OpcodesImplementation::Opcodes::op_fSub:
      *args[0] = *args[1] - (*args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_iMul:
    case OpcodesImplementation::Opcodes::op_fMul:
      *args[0] = *args[1] * (*args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_iDiv:
    case OpcodesImplementation::Opcodes::op_fDiv:
      *args[0] = *args[1] / (*args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_iMod:
      *args[0] = *args[1] % (*args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_Not:
      *args[0] = !(*args[1]);
      break;
    case OpcodesImplementation::Opcodes::op_iNeg:
      *args[0] = *args[1] * VarValue(-1);
      break;
    case OpcodesImplementation::Opcodes::op_fNeg:
      *args[0] = *args[1] * VarValue(-1.0f);
      break;
    case OpcodesImplementation::Opcodes::op_Assign:
      *args[0] = *args[1];
      break;
    case OpcodesImplementation::Opcodes::op_Cast:
      switch ((*args[0]).GetType()) {
        case VarValue::kType_Object: {
          auto to = args[0];
          auto from = IsSelfStr(*args[1]) ? &activeInstanceOwner : args[1];
          CastObjectToObject(*parentVM, to, from);
        } break;
        case VarValue::kType_Integer:
          *args[0] = (*args[1]).CastToInt();
          break;
        case VarValue::kType_Float:
          *args[0] = (*args[1]).CastToFloat();
          break;
        case VarValue::kType_Bool:
          *args[0] = (*args[1]).CastToBool();
          break;
        case VarValue::kType_String:
          *args[0] = VarValue::CastToString(*args[1]);
          break;
        default:
          // assert(0);
          // Triggered by some array stuff in SkyMP, not sure this is OK
          *args[0] = (*args[1]);
          break;
      }
      break;
    case OpcodesImplementation::op_Cmp_eq:
      *args[0] = VarValue((*args[1]) == (*args[2]));
      break;
    case OpcodesImplementation::Opcodes::op_Cmp_lt:
      *args[0] = VarValue(*args[1] < *args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_Cmp_le:
      *args[0] = VarValue(*args[1] <= *args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_Cmp_gt:
      *args[0] = VarValue(*args[1] > *args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_Cmp_ge:
      *args[0] = VarValue(*args[1] >= *args[2]);
      break;
    case OpcodesImplementation::Opcodes::op_Jmp:
      ctx->jumpStep = (int)(*args[0]) - 1;
      ctx->needJump = true;
      break;
    case OpcodesImplementation::Opcodes::op_Jmpt:
      if ((bool)(*args[0])) {
        ctx->jumpStep = (int)(*args[1]) - 1;
        ctx->needJump = true;
      }
      break;
    case OpcodesImplementation::Opcodes::op_Jmpf:
      if ((bool)(!(*args[0]))) {
        ctx->jumpStep = (int)(*args[1]) - 1;
        ctx->needJump = true;
      }
      break;
    case OpcodesImplementation::Opcodes::op_CallParent: {
      auto parentName =
        parentInstance ? parentInstance->GetSourcePexName() : "";
      auto gameObject = static_cast<IGameObject*>(activeInstanceOwner);

      std::vector<std::shared_ptr<ActivePexInstance>>
        activePexInstancesForCallParent;
      if (parentVM->IsStrictExecution()) {
        if (!parentInstance || !parentInstance->IsValid())
          throw std::runtime_error("Papyrus Parent has no script instance");
        activePexInstancesForCallParent.push_back(parentInstance);
      } else if (gameObject) {
        activePexInstancesForCallParent = gameObject->ListActivePexInstances();

        std::string toFind = sourcePex.source;

        for (auto& v : activePexInstancesForCallParent) {
          if (!Utils::stricmp(v->GetSourcePexName().data(), toFind.data())) {
            v = parentInstance;
            spdlog::trace("CallParent: redirecting method call {} -> {}",
                          toFind, parentName);
          }
        }
      }

      if (spdlog::should_log(spdlog::level::trace)) {
        std::vector<std::string> argsForCallStr;
        for (auto v : argsForCall) {
          argsForCallStr.push_back(v.ToString());
        }
        spdlog::trace("CallParent: calling with args {}",
                      fmt::join(argsForCallStr, ", "));
      }

      auto res =
        parentVM->CallMethod(gameObject, (const char*)(*args[0]), argsForCall,
                             ctx->stackData, &activePexInstancesForCallParent);
      if (EnsureCallResultIsSynchronous(res, ctx)) {
        *args[1] = res;
      }
    } break;
    case OpcodesImplementation::Opcodes::op_CallMethod: {
      VarValue* object = IsSelfStr(*args[1]) ? &activeInstanceOwner : args[1];
      if (parentVM->IsStrictExecution() &&
          object->GetType() != VarValue::kType_Object)
        throw std::runtime_error("Papyrus method target is not an object");

      // BYOHRelationshipAdoptionPetDoorTrigger in Skyrim Legendary Edition
      if (args[0]->GetType() != VarValue::kType_String &&
          args[0]->GetType() != VarValue::kType_Identifier) {
        if (parentVM->IsStrictExecution())
          throw std::runtime_error("Invalid Papyrus method name");
        *args[2] = VarValue::None();
        spdlog::error("OpcodesImplementation::Opcodes::op_CallMethod - "
                      "anomaly, string expected");
        break;
      }

      std::string functionName = (const char*)(*args[0]);
      static const std::string nameOnBeginState = "onBeginState";
      static const std::string nameOnEndState = "onEndState";

      if (!parentVM->IsStrictExecution() &&
          (functionName == nameOnBeginState ||
           functionName == nameOnEndState)) {
        // TODO: consider using CallMethod here. I'm afraid that this event
        // will pollute other scripts attached to an object
        parentVM->SendEvent(this, functionName.c_str(), argsForCall);
        break;
      } else {
        auto nullableGameObject = static_cast<IGameObject*>(*object);
        std::vector<std::shared_ptr<ActivePexInstance>> roots;
        const bool scopedSelf = parentVM->IsStrictExecution() &&
          (args[1] == &activeInstanceOwner || IsSelfStr(*args[1]));
        if (scopedSelf)
          roots.push_back(GetAttachedRoot());
        auto res = parentVM->CallMethod(
          nullableGameObject, functionName.c_str(), argsForCall,
          ctx->stackData, scopedSelf ? &roots : nullptr,
          scopedSelf ? nullptr : object->objectType.c_str());
        spdlog::trace("callmethod object={} funcName={} result={}",
                      object->ToString(), functionName, res.ToString());
        if (EnsureCallResultIsSynchronous(res, ctx)) {
          *args[2] = res;
        }
      }
    } break;
    case OpcodesImplementation::Opcodes::op_CallStatic: {
      const char* className = (const char*)(*args[0]);
      const char* functionName = (const char*)(*args[1]);
      auto res = parentVM->CallStatic(className, functionName, argsForCall,
                                      ctx->stackData);
      if (EnsureCallResultIsSynchronous(res, ctx)) {
        *args[2] = res;
      }
    } break;
    case OpcodesImplementation::Opcodes::op_Return:
      ctx->returnValue = *args[0];
      ctx->needReturn = true;
      break;
    case OpcodesImplementation::Opcodes::op_StrCat:
      *args[0] = OpcodesImplementation::StrCat(
        *args[1], *args[2], this->sourcePex.fn()->stringTable);
      break;
    case OpcodesImplementation::Opcodes::op_PropGet:
      // PropGet/Set seems to work only in very simple cases covered by unit
      // tests
      if (args[0] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 0 for Opcodes::op_PropGet");
      } else if (args[1] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 1 for Opcodes::op_PropGet");
      } else if (args[2] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 2 for Opcodes::op_PropGet");
      } else {
        std::string propertyName;

        if (args[0]->GetType() == VarValue::kType_String ||
            args[0]->GetType() == VarValue::kType_Identifier) {
          propertyName = static_cast<const char*>(*args[0]);
        } else {
          spdlog::error("Papyrus VM: argument with index 0 has unexpected "
                        "type {} for Opcodes::op_PropGet",
                        static_cast<int>(args[0]->GetType()));
        }

        IGameObject* object = nullptr;
        VarValue objectVarValue =
          IsSelfStr(*args[1]) ? activeInstanceOwner : *args[1];
        if (objectVarValue.GetType() == VarValue::kType_Object) {
          object = static_cast<IGameObject*>(objectVarValue);
        } else {
          spdlog::error("Papyrus VM: object has unexpected type {} for "
                        "Opcodes::op_PropGet",
                        static_cast<int>(objectVarValue.GetType()));
        }

        if (!object) {
          if (activeInstanceOwner.GetType() == VarValue::kType_Object) {
            object = static_cast<IGameObject*>(activeInstanceOwner);
          } else {
            spdlog::error(
              "Papyrus VM: activeInstanceOwner has unexpected type {} for "
              "Opcodes::op_PropGet",
              static_cast<int>(activeInstanceOwner.GetType()));
          }
        }

        if (object && object->ListActivePexInstances().size() > 0) {
          auto inst = object->ListActivePexInstances().back();
          Object::PropInfo* runProperty =
            GetProperty(*inst, propertyName, Object::PropInfo::kFlags_Read);
          if (runProperty != nullptr) {
            // TODO: use of argsForCall looks incorrect. why use argsForCall
            // here? shoud be {} (empty args)
            *args[2] = inst->StartFunction(runProperty->readHandler,
                                           argsForCall, ctx->stackData);
            spdlog::trace("propget function called");
          } else {
            auto& instProps = inst->sourcePex.fn()->objectTable[0].properties;
            auto it =
              std::find_if(instProps.begin(), instProps.end(),
                           [&](const Object::PropInfo& propInfo) {
                             return !Utils::stricmp(propInfo.name.data(),
                                                    propertyName.data());
                           });
            if (it == instProps.end()) {
              if (parentVM->IsStrictExecution())
                throw std::runtime_error("Unknown Papyrus property: " +
                                         propertyName);
              spdlog::trace("propget do nothing: prop {} not found",
                            propertyName);
            } else {

              VarValue* var;

              try {
                var = inst->variables->GetVariableByName(
                  it->autoVarName.data(), *inst->sourcePex.fn());

              } catch (std::exception& e) {
                if (parentVM->IsStrictExecution())
                  throw;
                spdlog::error("OpcodesImplementation::Opcodes::op_PropGet - "
                              "GetVariableByName errored with '{}'",
                              e.what());
                var = nullptr;
              } catch (...) {
                if (parentVM->IsStrictExecution())
                  throw;
                spdlog::critical(
                  "OpcodesImplementation::Opcodes::op_PropGet - "
                  "GetVariableByName errored with unknown error");
                var = nullptr;
                std::terminate();
              }

              if (var) {
                *args[2] = *var;
              } else {
                if (parentVM->IsStrictExecution())
                  throw std::runtime_error(
                    "Unknown Papyrus property variable");
                spdlog::trace("propget do nothing: variable {} not found",
                              it->autoVarName);
              }
            }
          }
          spdlog::trace("propget propName={} object={} result={}",
                        args[0]->ToString(), args[1]->ToString(),
                        args[2]->ToString());
        }
      }
      break;
    case OpcodesImplementation::Opcodes::op_PropSet:
      if (args[0] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 0 for Opcodes::op_PropSet");
      } else if (args[1] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 1 for Opcodes::op_PropSet");
      } else if (args[2] == nullptr) {
        spdlog::error(
          "Papyrus VM: null argument with index 2 for Opcodes::op_PropSet");
      } else {

        // TODO: use of argsForCall looks incorrect
        argsForCall.push_back(*args[2]);

        std::string propertyName;

        if (args[0]->GetType() == VarValue::kType_String ||
            args[0]->GetType() == VarValue::kType_Identifier) {
          propertyName = static_cast<const char*>(*args[0]);
        } else {
          spdlog::error("Papyrus VM: argument with index 0 has unexpected "
                        "type {} for Opcodes::op_PropSet",
                        static_cast<int>(args[0]->GetType()));
        }

        IGameObject* object = nullptr;
        VarValue objectVarValue =
          IsSelfStr(*args[1]) ? activeInstanceOwner : *args[1];
        if (objectVarValue.GetType() == VarValue::kType_Object) {
          object = static_cast<IGameObject*>(objectVarValue);
        } else {
          spdlog::error("Papyrus VM: object has unexpected type {} for "
                        "Opcodes::op_PropSet",
                        static_cast<int>(objectVarValue.GetType()));
        }

        if (!object) {
          if (activeInstanceOwner.GetType() == VarValue::kType_Object) {
            object = static_cast<IGameObject*>(activeInstanceOwner);
          } else {
            spdlog::error(
              "Papyrus VM: activeInstanceOwner has unexpected type {} for "
              "Opcodes::op_PropSet",
              static_cast<int>(activeInstanceOwner.GetType()));
          }
        }

        if (object && object->ListActivePexInstances().size() > 0) {
          auto inst = object->ListActivePexInstances().back();
          Object::PropInfo* runProperty =
            GetProperty(*inst, propertyName, Object::PropInfo::kFlags_Write);
          if (runProperty != nullptr) {
            // TODO: use of argsForCall looks incorrect.
            // probably should only *args[2]
            inst->StartFunction(runProperty->writeHandler, argsForCall,
                                ctx->stackData);
            spdlog::trace("propset function called");
          } else {
            auto& instProps = inst->sourcePex.fn()->objectTable[0].properties;
            auto it =
              std::find_if(instProps.begin(), instProps.end(),
                           [&](const Object::PropInfo& propInfo) {
                             return !Utils::stricmp(propInfo.name.data(),
                                                    propertyName.data());
                           });
            if (it == instProps.end()) {
              if (parentVM->IsStrictExecution())
                throw std::runtime_error("Unknown Papyrus property: " +
                                         propertyName);
              spdlog::trace("propset do nothing: prop {} not found",
                            propertyName);
            } else {
              VarValue* var;

              try {
                var = inst->variables->GetVariableByName(
                  it->autoVarName.data(), *inst->sourcePex.fn());
              } catch (std::exception& e) {
                if (parentVM->IsStrictExecution())
                  throw;
                spdlog::error("OpcodesImplementation::Opcodes::op_PropSet - "
                              "GetVariableByName errored with '{}'",
                              e.what());
                var = nullptr;
              } catch (...) {
                if (parentVM->IsStrictExecution())
                  throw;
                spdlog::critical(
                  "OpcodesImplementation::Opcodes::op_PropSet - "
                  "GetVariableByName errored with unknown error");
                var = nullptr;
                std::terminate();
              }

              if (var) {
                *var = *args[2];
              } else {
                if (parentVM->IsStrictExecution())
                  throw std::runtime_error(
                    "Unknown Papyrus property variable");
                spdlog::trace("propset do nothing: variable {} not found",
                              it->autoVarName);
              }
            }
          }
          spdlog::trace("propset propName={} object={} result={}",
                        args[0]->ToString(), args[1]->ToString(),
                        args[2]->ToString());
        }
      }
      break;
    case OpcodesImplementation::Opcodes::op_Array_Create:
      if (parentVM->IsStrictExecution() &&
          (static_cast<int32_t>(*args[1]) < 0 ||
           static_cast<int32_t>(*args[1]) > 1000000))
        throw std::runtime_error("Invalid Papyrus array length");
      (*args[0]).pArray = std::make_shared<std::vector<VarValue>>();
      if ((int32_t)(*args[1]) > 0) {
        (*args[0]).pArray->resize((int32_t)(*args[1]));
        uint8_t type = GetArrayElementType((*args[0]).GetType());
        for (auto& element : *(*args[0]).pArray) {
          element = VarValue(type);
        }
      } else {
        spdlog::warn("OpcodesImplementation::Opcodes::op_Array_Create - "
                     "Zero-size array creation attempt");
      }
      break;
    case OpcodesImplementation::Opcodes::op_Array_Length:
      if ((*args[1]).pArray != nullptr) {
        if ((*args[0]).GetType() == VarValue::kType_Integer) {
          *args[0] = VarValue((int32_t)(*args[1]).pArray->size());
        } else if ((*args[0]).GetType() == VarValue::kType_Float) {
          *args[0] = VarValue((double)(*args[1]).pArray->size());
        }
      } else {
        *args[0] = VarValue((int32_t)0);
      }
      break;
    case OpcodesImplementation::Opcodes::op_Array_GetElement:
      if ((*args[1]).pArray != nullptr) {
        const int index = static_cast<int>(*args[2]);
        const auto indexType = (*args[2]).GetType();
        if ((indexType == VarValue::kType_Integer ||
             indexType == VarValue::kType_Float) &&
            index >= 0 && index < (*args[1]).pArray->size()) {
          *args[0] = (*args[1]).pArray->operator[](index);
        } else {
          *args[0] = VarValue::None();
          spdlog::error("OpcodesImplementation::Opcodes::op_Array_GetElement "
                        "- Invalid array index");
        }
      } else {
        *args[0] = VarValue::None();
      }
      break;
    case OpcodesImplementation::Opcodes::op_Array_SetElement:
      if ((*args[0]).pArray != nullptr) {
        const int index = static_cast<int>(*args[1]);
        const auto indexType = (*args[1]).GetType();
        if ((indexType == VarValue::kType_Integer ||
             indexType == VarValue::kType_Float) &&
            index >= 0 && index < (*args[0]).pArray->size()) {
          (*args[0]).pArray->operator[](index) = *args[2];
        } else {
          spdlog::error("OpcodesImplementation::Opcodes::op_Array_SetElement "
                        "- Invalid array index");
        }
      } else {
        spdlog::error("OpcodesImplementation::Opcodes::op_Array_SetElement - "
                      "null array passed");
      }
      break;
    case OpcodesImplementation::Opcodes::op_Array_FindElement:
      OpcodesImplementation::ArrayFindElement(*args[0], *args[1], *args[2],
                                              *args[3]);
      break;
    case OpcodesImplementation::Opcodes::op_Array_RfindElement:
      OpcodesImplementation::ArrayRFindElement(*args[0], *args[1], *args[2],
                                               *args[3]);
      break;
    default:
      if (parentVM->IsStrictExecution())
        throw std::runtime_error("Unsupported Papyrus opcode");
      assert(0);
  }
}

std::shared_ptr<std::vector<ActivePexInstance::Local>>
ActivePexInstance::MakeLocals(const FunctionInfo& function,
                              const std::vector<VarValue>& arguments)
{
  auto locals = std::make_shared<std::vector<Local>>();

  // Fill with function locals
  for (auto& var : function.locals) {
    VarValue temp = VarValue(GetTypeByName(var.type));
    temp.objectType = var.type;
    locals->push_back({ var.name, temp });
  }

  // Fill with function args
  for (size_t i = 0; i < arguments.size(); ++i) {
    VarValue temp = arguments[i];
    temp.objectType = function.params[i].type;

    locals->push_back({ function.params[i].name, temp });
    assert(locals->back().second.GetType() == arguments[i].GetType());
  }

  // ?
  for (size_t i = arguments.size(); i < function.params.size(); ++i) {
    const auto& var_ = function.params[i];

    VarValue temp = VarValue(GetTypeByName(var_.type));
    temp.objectType = var_.type;

    locals->push_back({ var_.name, temp });
  }

  // Dereference identifiers
  for (auto& var : *locals) {
    var.second = GetIndentifierValue(*locals, var.second);
  }

  return locals;
}

// Basically, makes vector<VarValue *> from vector<VarValue>
std::vector<std::pair<uint8_t, std::vector<VarValue*>>>
ActivePexInstance::TransformInstructions(
  std::vector<FunctionCode::Instruction>& instructions,
  std::shared_ptr<std::vector<Local>> locals)
{
  std::vector<std::pair<uint8_t, std::vector<VarValue*>>> opCode;
  for (size_t i = 0; i < instructions.size(); ++i) {

    std::pair<uint8_t, std::vector<VarValue*>> temp;
    temp.first = instructions[i].op;

    for (size_t j = 0; j < instructions[i].args.size(); ++j) {
      temp.second.push_back(&instructions[i].args[j]);
    }
    opCode.push_back(temp);
  }

  // Dereference identifiers
  for (auto& [opcodeId, opcodeArgs] : opCode) {
    size_t dereferenceStart;
    switch (opcodeId) {
      case OpcodesImplementation::Opcodes::op_CallMethod:
        // Do not dereference functionName
        dereferenceStart = 1;
        break;
      case OpcodesImplementation::Opcodes::op_CallStatic:
        // Do not dereference className and functionName
        dereferenceStart = 2;
        break;
      case OpcodesImplementation::Opcodes::op_CallParent:
        // Do not dereference functionName
        dereferenceStart = 1;
        break;
      case OpcodesImplementation::Opcodes::op_PropGet:
      case OpcodesImplementation::Opcodes::op_PropSet:
        // Do not dereference property name
        dereferenceStart = 1;
        break;
      default:
        dereferenceStart = 0;
        break;
    }
    for (size_t i = dereferenceStart; i < opcodeArgs.size(); ++i) {
      auto& arg = opcodeArgs[i];
      arg = &(GetIndentifierValue(*locals, *arg));
    }
  }

  return opCode;
}

VarValue ActivePexInstance::ExecuteAll(
  ExecutionContext& ctx, std::optional<VarValue> previousCallResult)
{
  auto pipex = sourcePex.fn();

  auto opCode = TransformInstructions(ctx.instructions, ctx.locals);

  if (previousCallResult) {
    int i = ctx.line - 1;
    size_t resultIdx =
      opCode[i].first == OpcodesImplementation::Opcodes::op_CallParent ? 1 : 2;

    if (opCode[i].first != FunctionCode::kOp_PropSet)
      *opCode[i].second[resultIdx] = *previousCallResult;
  }

  // TODO: log and handle this
  assert(opCode.size() == ctx.instructions.size());

  constexpr static size_t kOpCodeExecutionsQuota = 100'000;

  size_t opCodeExecutions = 0;

  for (; ctx.line < opCode.size(); ++ctx.line) {
    parentVM->CheckExecutionBudget();

    if (opCodeExecutions >= kOpCodeExecutionsQuota) {
      if (parentVM->IsStrictExecution())
        throw std::runtime_error("Papyrus instruction quota exceeded");
      spdlog::error("ActivePexInstance::ExecuteAll - Quota exceeded in script "
                    "{}, returning None",
                    sourcePex.fn()->source);
      return VarValue::None();
    }

    auto& [op, args] = opCode[ctx.line];
    try {
      ExecuteOpCode(&ctx, op, args);
    } catch (const VirtualMachine::SuspensionRequest& request) {
      if (!parentVM->checkpointFrame || !parentVM->activeFrames ||
          parentVM->activeFrames > 128)
        throw std::runtime_error("Unsupported Papyrus suspension boundary");
      auto frame = DescribeFrame(ctx.identity);
      frame.resultType = parentVM->checkpointSuspended
        ? parentVM->suspendedReturnType
        : request.resultType;
      if (frame.resultType.empty() ||
          !Utils::stricmp(frame.resultType.c_str(), "identifier"))
        throw std::runtime_error(
          "Invalid Papyrus suspension result declaration");
      const VarValue prototype(GetTypeByName(frame.resultType));
      ValidateFrameValue(prototype, frame.resultType);
      ValidateCallResult(ctx.instructions.at(ctx.line), *ctx.locals,
                         prototype);
      for (const auto& local : *ctx.locals)
        ValidateFrameValue(local.second, local.second.objectType, true);
      frame.instruction = static_cast<uint32_t>(ctx.line);
      frame.locals = *ctx.locals;
      if (!parentVM->checkpointSuspended) {
        if (op != FunctionCode::kOp_CallMethod &&
            op != FunctionCode::kOp_CallStatic &&
            op != FunctionCode::kOp_CallParent)
          throw std::runtime_error("Unsupported native suspension boundary");
        PapyrusContinuation pending;
        static_cast<PapyrusFrame&>(pending) = std::move(frame);
        pending.scope = parentVM->checkpointFrame->scope;
        pending.token = request.token;
        pending.executedInstructions =
          static_cast<uint32_t>(parentVM->executedInstructions);
        parentVM->checkpointFrame = std::move(pending);
      } else {
        if (parentVM->checkpointFrame->callers.size() >= 127)
          throw std::runtime_error(
            "Papyrus continuation caller limit exceeded");
        parentVM->checkpointFrame->callers.push_back(std::move(frame));
      }
      parentVM->checkpointSuspended = true;
      parentVM->suspendedReturnType = ctx.returnType;
      // Unwind only to the explicit host entry. Every Papyrus caller captures
      // its own call site; no C++ callback or interpreter address is retained.
      throw;
    }

    ++opCodeExecutions;

    if (ctx.needReturn) {
      ctx.needReturn = false;
      return ctx.returnValue;
    }

    if (ctx.needJump) {
      ctx.needJump = false;
      const auto next = static_cast<int64_t>(ctx.line) + ctx.jumpStep + 1;
      if (parentVM->IsStrictExecution() &&
          (next < 0 || next > static_cast<int64_t>(opCode.size())))
        throw std::runtime_error("Papyrus jump is outside function");
      ctx.line += ctx.jumpStep;
    }
  }
  return ctx.returnValue;
}

void ActivePexInstance::ValidateCheckpointEntry(const std::string& scope) const
{
  if (!parentVM || !parentVM->IsStrictExecution() || parentVM->activeFrames ||
      parentVM->checkpointFrame || !IsValid() || scope.empty() ||
      scope.size() > 300 || scope.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid Papyrus checkpoint entry");
}

FunctionInfo ActivePexInstance::FrameFunction(
  const PapyrusFunctionIdentity& identity) const
{
  FunctionInfo function;
  if (identity.kind == "method" || identity.kind == "static") {
    if (identity.kind == "static" && !identity.state.empty())
      throw std::runtime_error("Static continuation has a state");
    function = GetFunctionByName(identity.function.c_str(), identity.state);
  } else if (identity.kind == "propertyGet" ||
             identity.kind == "propertySet") {
    if (!identity.state.empty())
      throw std::runtime_error("Property continuation has a state");
    const bool write = identity.kind == "propertySet";
    size_t found = 0;
    for (const auto& property : sourcePex.fn()->objectTable.at(0).properties) {
      if (Utils::stricmp(property.name.c_str(), identity.function.c_str()))
        continue;
      const auto access =
        write ? Object::PropInfo::kFlags_Write : Object::PropInfo::kFlags_Read;
      if (!(property.flags & access) ||
          (property.flags & Object::PropInfo::kFlags_AutoVar))
        throw std::runtime_error("Unavailable continuation property handler");
      function = write ? property.writeHandler : property.readHandler;
      function.valid = true;
      if (++found != 1)
        throw std::runtime_error("Ambiguous continuation property handler");
    }
  } else {
    throw std::runtime_error("Unknown Papyrus continuation function kind");
  }
  if (!function.valid || function.IsNative() ||
      function.IsGlobal() != (identity.kind == "static"))
    throw std::runtime_error("Invalid resumable Papyrus function");
  return function;
}

PapyrusFrame ActivePexInstance::DescribeFrame(
  const PapyrusFunctionIdentity& identity) const
{
  if (!parentVM || !parentVM->IsStrictExecution() || !IsValid() ||
      identity.state.size() > 1024 ||
      identity.state.find('\0') != std::string::npos ||
      identity.function.empty() || identity.function.size() > 1024 ||
      identity.function.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid Papyrus frame identity");
  FrameFunction(identity);
  PapyrusFrame frame;
  frame.kind = identity.kind;
  frame.receiver = activeInstanceOwner;
  frame.script = GetSourcePexName();
  frame.state = identity.state;
  frame.function = identity.function;
  const ActivePexInstance* root = this;
  std::shared_ptr<ActivePexInstance> attachedRoot;
  if (identity.kind == "static") {
    if (activeInstanceOwner.GetType() != VarValue::kType_Object ||
        static_cast<IGameObject*>(activeInstanceOwner))
      throw std::runtime_error("Static continuation has an attached receiver");
  } else {
    if (activeInstanceOwner.GetType() != VarValue::kType_Object ||
        !static_cast<IGameObject*>(activeInstanceOwner))
      throw std::runtime_error(
        "Papyrus checkpoint requires an attached receiver");
    attachedRoot = GetAttachedRoot();
    root = attachedRoot.get();
    size_t matches = 0;
    for (const auto& attachment :
         static_cast<IGameObject*>(activeInstanceOwner)
           ->ListActivePexInstances())
      if (attachment && attachment->IsValid() &&
          !Utils::stricmp(attachment->GetSourcePexName().c_str(),
                          root->GetSourcePexName().c_str()))
        ++matches;
    if (matches != 1)
      throw std::runtime_error("Ambiguous Papyrus checkpoint attachment");
  }
  frame.rootScript = root->GetSourcePexName();
  for (auto* entry = root; entry && entry->IsValid();
       entry = entry->parentInstance.get()) {
    if (entry->parentVM != parentVM || frame.contentHashes.size() >= 128)
      throw std::runtime_error("Invalid Papyrus checkpoint inheritance chain");
    frame.contentHashes.emplace_back(entry->GetSourcePexName(),
                                     entry->sourcePex.fn()->ExecutionHash());
  }
  return frame;
}

void ActivePexInstance::ValidateResumableFunction(
  const FunctionInfo& function, const std::vector<VarValue>& arguments,
  const PapyrusFunctionIdentity& identity) const
{
  FrameFunction(identity);
  if (!function.valid || function.IsNative() ||
      function.IsGlobal() != (identity.kind == "static") ||
      function.params.size() != arguments.size() ||
      function.params.size() + function.locals.size() > 65535 ||
      function.code.instructions.size() > 100000 ||
      function.returnType.empty() || function.returnType.size() > 1024 ||
      function.returnType.find('\0') != std::string::npos ||
      !Utils::stricmp(function.returnType.c_str(), "identifier"))
    throw std::runtime_error("Invalid resumable Papyrus function");
  std::set<CIString> names;
  for (const auto* declarations : { &function.locals, &function.params })
    for (const auto& declaration : *declarations) {
      if (declaration.name.empty() || declaration.name.size() > 1024 ||
          declaration.name.find('\0') != std::string::npos ||
          !Utils::stricmp(declaration.name.c_str(), "self") ||
          !Utils::stricmp(declaration.name.c_str(), "::State") ||
          !names
             .insert(
               CIString{ declaration.name.begin(), declaration.name.end() })
             .second ||
          declaration.type.empty() || declaration.type.size() > 1024 ||
          declaration.type.find('\0') != std::string::npos ||
          !Utils::stricmp(declaration.type.c_str(), "identifier"))
        throw std::runtime_error(
          "Invalid resumable Papyrus local declaration");
    }
  for (size_t i = 0; i < arguments.size(); ++i)
    ValidateFrameValue(arguments[i], function.params[i].type);
}

void ActivePexInstance::ValidateCallResult(
  const FunctionCode::Instruction& call, std::vector<Local>& locals,
  const VarValue& completion)
{
  if (call.op == FunctionCode::kOp_PropSet) {
    ValidateFrameValue(completion, "None");
    return;
  }
  if (call.op != FunctionCode::kOp_CallMethod &&
      call.op != FunctionCode::kOp_CallStatic &&
      call.op != FunctionCode::kOp_CallParent &&
      call.op != FunctionCode::kOp_PropGet)
    throw std::runtime_error(
      "Papyrus continuation is not at a call instruction");
  const size_t destination = call.op == FunctionCode::kOp_CallParent ? 1 : 2;
  if (call.args.size() <= destination ||
      call.args[destination].GetType() != VarValue::kType_Identifier ||
      !static_cast<const char*>(call.args[destination]))
    throw std::runtime_error(
      "Invalid Papyrus continuation result destination");
  const std::string name = static_cast<const char*>(call.args[destination]);
  if (name.empty() || name.size() > 1024 ||
      !Utils::stricmp(name.c_str(), "self") ||
      !Utils::stricmp(name.c_str(), "::State"))
    throw std::runtime_error("Invalid Papyrus continuation result target");
  const auto& slot = GetVariableValueByName(&locals, name);
  ValidateFrameValue(completion, slot.objectType, true);
}

std::shared_ptr<ActivePexInstance> ActivePexInstance::ResolveFrame(
  const PapyrusFrame& frame)
{
  if (frame.receiver.GetType() != VarValue::kType_Object ||
      frame.receiver.promise)
    throw std::runtime_error("Invalid Papyrus continuation receiver");
  if (frame.kind == "static") {
    if (static_cast<IGameObject*>(frame.receiver) ||
        frame.rootScript != frame.script)
      throw std::runtime_error("Invalid static Papyrus continuation identity");
    auto& instance = parentVM->instancesForStaticCalls[frame.script];
    if (!instance)
      instance = parentVM->CreateActivePexInstance(
        frame.script, VarValue::None(), nullptr, "");
    if (!instance || !instance->IsValid())
      throw std::runtime_error("Missing static Papyrus continuation script");
    return instance;
  }
  auto* object = static_cast<IGameObject*>(frame.receiver);
  if (!object)
    throw std::runtime_error("Missing Papyrus continuation receiver");
  std::shared_ptr<ActivePexInstance> result;
  for (const auto& root : object->ListActivePexInstances()) {
    if (!root || !root->IsValid() ||
        Utils::stricmp(root->GetSourcePexName().c_str(),
                       frame.rootScript.c_str()))
      continue;
    for (auto instance = root; instance && instance->IsValid();
         instance = instance->GetParentInstance()) {
      if (instance->parentVM != parentVM)
        throw std::runtime_error(
          "Continuation instance belongs to another VM");
      if (Utils::stricmp(instance->GetSourcePexName().c_str(),
                         frame.script.c_str()))
        continue;
      if (result)
        throw std::runtime_error(
          "Ambiguous Papyrus continuation implementation");
      result = instance;
    }
  }
  if (!result)
    throw std::runtime_error("Missing Papyrus continuation implementation");
  return result;
}

void ActivePexInstance::ValidateFrameValue(const VarValue& value,
                                           const std::string& type,
                                           bool discard) const
{
  if (type.empty() || type.size() > 1024 ||
      type.find('\0') != std::string::npos ||
      !Utils::stricmp(type.c_str(), "identifier") || value.promise)
    throw std::runtime_error("Invalid Papyrus checkpoint value type");
  const auto actual = value.GetType();
  if (actual == VarValue::kType_Identifier ||
      (actual > VarValue::kType_Bool &&
       (actual < VarValue::kType_ObjectArray ||
        actual > VarValue::kType_BoolArray)) ||
      (actual == VarValue::kType_Float &&
       !std::isfinite(static_cast<double>(value))) ||
      (actual == VarValue::kType_String && !static_cast<const char*>(value)))
    throw std::runtime_error("Invalid Papyrus checkpoint value");
  const bool none = !Utils::stricmp(type.c_str(), "None");
  const auto expected = GetTypeByName(type);
  const bool null =
    actual == VarValue::kType_Object && !static_cast<IGameObject*>(value);
  // The compiler's None temporary may discard a native's nonvoid result.
  if (!(discard && none) && actual != expected &&
      !(null && expected >= VarValue::kType_ObjectArray))
    throw std::runtime_error(
      "Papyrus checkpoint value does not match declaration");
  if (none && !discard && !null)
    throw std::runtime_error("Papyrus completion must be None");
  if (actual == VarValue::kType_Object && !null && !none &&
      !parentVM->DynamicCast(value, CIString{ type.begin(), type.end() }))
    throw std::runtime_error("Papyrus checkpoint object has the wrong type");
  if (actual >= VarValue::kType_ObjectArray && value.pArray) {
    if (value.pArray->size() > 1000000)
      throw std::runtime_error("Papyrus checkpoint array exceeds limits");
    auto element = type.size() > 2 && type.substr(type.size() - 2) == "[]"
      ? type.substr(0, type.size() - 2)
      : std::string{};
    if (discard && none) {
      static const char* elementTypes[] = { "None", "String", "Int", "Float",
                                            "Bool" };
      element = elementTypes[actual - VarValue::kType_ObjectArray];
    }
    if (element.empty() || element.find("[]") != std::string::npos)
      throw std::runtime_error("Invalid Papyrus checkpoint array declaration");
    for (const auto& entry : *value.pArray) {
      // Papyrus arrays cannot contain arrays. Check the physical element tag
      // even for a compiler discard local, before any recursive validation.
      if (entry.GetType() != GetArrayElementType(actual))
        throw std::runtime_error(
          "Invalid Papyrus checkpoint array element type");
      ValidateFrameValue(entry, element, discard && none);
    }
  }
}

PapyrusExecutionResult ActivePexInstance::StartResumableFunction(
  const std::string& scope, const std::string& state,
  const std::string& functionName, std::vector<VarValue> arguments,
  std::shared_ptr<StackData> stackData)
{
  ValidateCheckpointEntry(scope);
  const PapyrusFunctionIdentity identity{ "method", state, functionName };
  const auto description = DescribeFrame(identity);
  const auto function = FrameFunction(identity);
  ValidateResumableFunction(function, arguments, identity);
  if (!stackData)
    throw std::runtime_error("Missing Papyrus checkpoint stack context");
  PapyrusContinuation seed;
  static_cast<PapyrusFrame&>(seed) = description;
  seed.scope = scope;
  parentVM->checkpointFrame = std::move(seed);
  parentVM->checkpointSuspended = false;
  parentVM->checkpointLocals = 0;
  Viet::ScopedTask<VirtualMachine*> checkpointGuard(
    [](VirtualMachine*& vm) {
      vm->checkpointFrame.reset();
      vm->checkpointSuspended = false;
      vm->checkpointLocals = 0;
      vm->suspendedReturnType.clear();
    },
    parentVM);
  PapyrusExecutionResult result;
  try {
    result.value =
      StartFunction(function, arguments, std::move(stackData), identity);
    ValidateFrameValue(result.value, function.returnType);
  } catch (const VirtualMachine::SuspensionRequest&) {
    if (!parentVM->checkpointSuspended)
      throw std::runtime_error("Incomplete Papyrus suspension capture");
    result.continuation = std::move(parentVM->checkpointFrame);
  }
  return result;
}

PapyrusExecutionResult ActivePexInstance::ResumeFunction(
  const std::string& scope, const PapyrusContinuation& saved,
  const VarValue& completion, std::shared_ptr<StackData> stackData)
{
  ValidateCheckpointEntry(scope);
  if (!stackData || saved.scope != scope || saved.token.empty() ||
      saved.token.size() > 256 ||
      saved.token.find('\0') != std::string::npos ||
      !saved.executedInstructions || saved.executedInstructions > 100000 ||
      saved.callers.size() > 127)
    throw std::runtime_error("Invalid Papyrus continuation context");
  std::vector<const PapyrusFrame*> frames{ &saved };
  size_t totalLocals = saved.locals.size();
  for (const auto& caller : saved.callers) {
    totalLocals += caller.locals.size();
    frames.push_back(&caller);
  }
  if (totalLocals > 65535)
    throw std::runtime_error("Papyrus continuation locals exceed stack limit");
  struct Prepared
  {
    std::shared_ptr<ActivePexInstance> instance;
    FunctionInfo function;
    std::shared_ptr<std::vector<Local>> locals;
  };
  std::vector<Prepared> prepared;
  // Preflight the entire stack before running the leaf. A changed/corrupt
  // outer caller must not be discovered only after the leaf has made effects.
  for (const auto* frame : frames) {
    auto instance = ResolveFrame(*frame);
    const PapyrusFunctionIdentity identity{ frame->kind, frame->state,
                                            frame->function };
    const auto current = instance->DescribeFrame(identity);
    if (frame->rootScript != current.rootScript ||
        frame->script != current.script ||
        frame->contentHashes != current.contentHashes ||
        !(frame->receiver == current.receiver) ||
        !(current.receiver == frame->receiver) || frame->resultType.empty() ||
        frame->resultType.size() > 1024 ||
        frame->resultType.find('\0') != std::string::npos ||
        !Utils::stricmp(frame->resultType.c_str(), "identifier"))
      throw std::runtime_error(
        "Papyrus continuation identity/content mismatch");
    auto function = instance->FrameFunction(identity);
    if (function.code.instructions.size() > 100000 ||
        frame->instruction >= function.code.instructions.size() ||
        frame->locals.size() !=
          function.locals.size() + function.params.size())
      throw std::runtime_error("Invalid Papyrus continuation function/frame");
    if (std::max<size_t>(parentVM->executedInstructions,
                         saved.executedInstructions) >= 100000 &&
        size_t(frame->instruction) + 1 < function.code.instructions.size())
      throw std::runtime_error(
        "Papyrus transaction instruction quota exceeded");
    std::set<CIString> names;
    auto locals = std::make_shared<std::vector<Local>>();
    for (size_t i = 0; i < frame->locals.size(); ++i) {
      const auto& declaration = i < function.locals.size()
        ? function.locals[i]
        : function.params[i - function.locals.size()];
      const auto& [name, value] = frame->locals[i];
      if (name != declaration.name || name.empty() || name.size() > 1024 ||
          name.find('\0') != std::string::npos ||
          !Utils::stricmp(name.c_str(), "self") ||
          !Utils::stricmp(name.c_str(), "::State") ||
          !names.insert(CIString{ name.begin(), name.end() }).second ||
          value.objectType != declaration.type)
        throw std::runtime_error("Papyrus continuation local layout mismatch");
      instance->ValidateFrameValue(value, declaration.type, true);
      locals->emplace_back(name, value);
    }
    const VarValue prototype(GetTypeByName(frame->resultType));
    instance->ValidateFrameValue(prototype, frame->resultType);
    instance->ValidateCallResult(
      function.code.instructions[frame->instruction], *locals, prototype);
    prepared.push_back(
      { std::move(instance), std::move(function), std::move(locals) });
  }
  if (prepared.back().instance.get() != this ||
      frames.back()->kind != "method")
    throw std::runtime_error(
      "Papyrus continuation entry belongs to another attachment");
  const auto leafOpcode =
    prepared.front().function.code.instructions[saved.instruction].op;
  if (leafOpcode != FunctionCode::kOp_CallMethod &&
      leafOpcode != FunctionCode::kOp_CallStatic &&
      leafOpcode != FunctionCode::kOp_CallParent)
    throw std::runtime_error("Invalid native continuation leaf boundary");
  for (size_t i = 1; i < frames.size(); ++i) {
    const auto& child = *frames[i - 1];
    const auto& caller = *frames[i];
    const auto& call =
      prepared[i].function.code.instructions[caller.instruction];
    auto word = [&](size_t index) -> std::string {
      const auto& value = call.args.at(index);
      if ((value.GetType() != VarValue::kType_Identifier &&
           value.GetType() != VarValue::kType_String) ||
          !static_cast<const char*>(value))
        throw std::runtime_error("Invalid Papyrus caller link name");
      return static_cast<const char*>(value);
    };
    const size_t functionIndex =
      call.op == FunctionCode::kOp_CallStatic ? 1 : 0;
    if (Utils::stricmp(word(functionIndex).c_str(), child.function.c_str()) ||
        Utils::stricmp(caller.resultType.c_str(),
                       prepared[i - 1].function.returnType.c_str()))
      throw std::runtime_error("Papyrus continuation caller link mismatch");
    if (call.op == FunctionCode::kOp_CallStatic) {
      if (child.kind != "static" ||
          Utils::stricmp(word(0).c_str(), child.script.c_str()))
        throw std::runtime_error(
          "Papyrus continuation static caller mismatch");
    } else if (call.op == FunctionCode::kOp_PropGet ||
               call.op == FunctionCode::kOp_PropSet) {
      if (child.kind !=
          (call.op == FunctionCode::kOp_PropGet ? "propertyGet"
                                                : "propertySet"))
        throw std::runtime_error(
          "Papyrus continuation property caller mismatch");
    } else if (child.kind != "method") {
      throw std::runtime_error("Papyrus continuation method caller mismatch");
    }
    if (call.op == FunctionCode::kOp_CallParent) {
      bool ancestor = false;
      for (auto entry = prepared[i].instance->GetParentInstance();
           entry && entry->IsValid(); entry = entry->GetParentInstance())
        ancestor |= entry.get() == prepared[i - 1].instance.get();
      if (!ancestor || !(child.receiver == caller.receiver) ||
          !(caller.receiver == child.receiver) ||
          child.rootScript != caller.rootScript)
        throw std::runtime_error(
          "Papyrus continuation parent caller mismatch");
    }
    if (call.op == FunctionCode::kOp_CallMethod ||
        call.op == FunctionCode::kOp_PropGet ||
        call.op == FunctionCode::kOp_PropSet) {
      const auto& operand = call.args.at(1);
      const VarValue* target = nullptr;
      bool self = IsSelfStr(operand);
      if (operand.GetType() == VarValue::kType_Identifier) {
        const char* name = static_cast<const char*>(operand);
        if (!name)
          throw std::runtime_error("Invalid Papyrus caller receiver operand");
        self = !Utils::stricmp(name, "self");
        for (const auto& local : caller.locals)
          if (!Utils::stricmp(local.first.c_str(), name))
            target = &local.second;
      } else if (!self) {
        target = &operand;
      }
      if (self) {
        target = &caller.receiver;
        if (child.rootScript != caller.rootScript)
          throw std::runtime_error(
            "Papyrus continuation self attachment mismatch");
        if (call.op == FunctionCode::kOp_PropGet ||
            call.op == FunctionCode::kOp_PropSet) {
          bool reachable = false;
          for (auto entry = prepared[i].instance; entry && entry->IsValid();
               entry = entry->GetParentInstance())
            reachable |= entry.get() == prepared[i - 1].instance.get();
          if (!reachable)
            throw std::runtime_error(
              "Papyrus continuation self property implementation mismatch");
        }
      }
      // A paused caller's locals and self cannot change while its callee runs.
      // A script field can: the callee may replace that field before awaiting.
      // Keep the captured child receiver in that case instead of
      // redispatching.
      if (target &&
          (target->GetType() != VarValue::kType_Object ||
           !(*target == child.receiver) || !(child.receiver == *target)))
        throw std::runtime_error(
          "Papyrus continuation caller receiver mismatch");
    }
  }
  prepared.front().instance->ValidateFrameValue(completion, saved.resultType);
  parentVM->checkpointFrame = saved;
  parentVM->checkpointSuspended = false;
  parentVM->executedInstructions = std::max<size_t>(
    parentVM->executedInstructions, saved.executedInstructions);
  Viet::ScopedTask<VirtualMachine*> checkpointGuard(
    [](VirtualMachine*& vm) {
      vm->activeFrames = 0;
      vm->checkpointLocals = 0;
      vm->checkpointFrame.reset();
      vm->checkpointSuspended = false;
      vm->suspendedReturnType.clear();
    },
    parentVM);
  VarValue returned = completion;
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = *frames[i];
    auto& ready = prepared[i];
    ready.instance->ValidateFrameValue(returned, frame.resultType);
    ready.instance->ValidateCallResult(
      ready.function.code.instructions[frame.instruction], *ready.locals,
      returned);
    if (parentVM->executedInstructions >= 100000 &&
        size_t(frame.instruction) + 1 <
          ready.function.code.instructions.size())
      throw std::runtime_error(
        "Papyrus transaction instruction quota exceeded");
    parentVM->activeFrames = frames.size() - i;
    parentVM->checkpointLocals = totalLocals;
    ExecutionContext context{ stackData, ready.function.code.instructions,
                              ready.locals };
    context.line = size_t(frame.instruction) + 1;
    context.identity = { frame.kind, frame.state, frame.function };
    context.returnType = ready.function.returnType;
    try {
      returned = ready.instance->ExecuteAll(context, returned);
      ready.instance->ValidateFrameValue(returned, ready.function.returnType);
    } catch (const VirtualMachine::SuspensionRequest&) {
      if (!parentVM->checkpointSuspended)
        throw std::runtime_error("Incomplete resumed Papyrus suspension");
      auto& captured = parentVM->checkpointFrame->callers;
      if (captured.size() + frames.size() - i - 1 > 127)
        throw std::runtime_error("Papyrus continuation caller limit exceeded");
      for (size_t pending = i + 1; pending < frames.size(); ++pending)
        captured.push_back(*frames[pending]);
      PapyrusExecutionResult result;
      result.continuation = std::move(parentVM->checkpointFrame);
      return result;
    }
    totalLocals -= frame.locals.size();
  }
  PapyrusExecutionResult result;
  result.value = returned;
  return result;
}

VarValue ActivePexInstance::StartFunction(
  const FunctionInfo& function, std::vector<VarValue>& arguments,
  std::shared_ptr<StackData> stackData,
  const PapyrusFunctionIdentity& identity)
{
  if (!stackData) {
    if (parentVM->IsStrictExecution())
      throw std::runtime_error("Missing Papyrus stack context");
    spdlog::error("ActivePexInstance::StartFunction - An empty stackData "
                  "passed to StartFunction");
    return VarValue::None();
  }

  if (parentVM->IsStrictExecution() && function.IsNative())
    throw std::runtime_error(
      "Native Papyrus function requires a registered implementation");

  const size_t localCount = parentVM->checkpointFrame
    ? function.locals.size() + function.params.size()
    : 0;
  if (parentVM->checkpointFrame) {
    if (localCount > 65535 || parentVM->checkpointLocals > 65535 - localCount)
      throw std::runtime_error(
        "Papyrus continuation locals exceed stack limit");
    if (!identity.kind.empty())
      ValidateResumableFunction(function, arguments, identity);
  }
  parentVM->checkpointLocals += localCount;
  std::pair<VirtualMachine*, size_t> localBudget{ parentVM, localCount };
  Viet::ScopedTask<decltype(localBudget)> localGuard(
    [](decltype(localBudget)& budget) {
      budget.first->checkpointLocals -= budget.second;
    },
    localBudget);

  if (parentVM->IsStrictExecution() && parentVM->activeFrames >= 128)
    throw std::runtime_error("Papyrus frame depth exceeded");
  ++parentVM->activeFrames;
  Viet::ScopedTask<VirtualMachine*> frameDepthGuard(
    [](VirtualMachine*& vm) { --vm->activeFrames; }, parentVM);

  thread_local StackDepthHolder g_stackDepthHolder;
  if (parentVM->IsStrictExecution() &&
      arguments.size() != function.params.size())
    throw std::runtime_error("Papyrus argument count mismatch");

  g_stackDepthHolder.IncreaseStackDepth();

  Viet::ScopedTask<StackDepthHolder> stackDepthDecreaseTask(
    [](StackDepthHolder& stackDepthHolder) {
      stackDepthHolder.DecreaseStackDepth();
    },
    g_stackDepthHolder);

  constexpr size_t kMaxStackDepth = 128;

  if (g_stackDepthHolder.GetStackDepth() > kMaxStackDepth) {
    if (parentVM->IsStrictExecution())
      throw std::runtime_error("Papyrus stack depth exceeded");
    spdlog::error("ActivePexInstance::StartFunction - Stack overflow in "
                  "script {}, returning None",
                  sourcePex.fn()->source);
    return VarValue::None();
  }

  auto locals = MakeLocals(function, arguments);
  ExecutionContext ctx{ stackData, function.code.instructions, locals };
  ctx.identity = identity;
  ctx.returnType = function.returnType;
  auto result = ExecuteAll(ctx);
  if (parentVM->checkpointFrame && !identity.kind.empty())
    ValidateFrameValue(result, function.returnType);
  return result;
}

VarValue& ActivePexInstance::GetIndentifierValue(
  std::vector<Local>& locals, VarValue& value, bool treatStringsAsIdentifiers)
{
  if (const char* valueAsString = static_cast<const char*>(value)) {
    if (treatStringsAsIdentifiers &&
        value.GetType() == VarValue::kType_String) {
      auto& res = GetVariableValueByName(&locals, valueAsString);
      return res;
    }
    if (value.GetType() == VarValue::kType_Identifier) {
      auto& res = GetVariableValueByName(&locals, valueAsString);
      return res;
    }
  }
  return value;
}

uint8_t ActivePexInstance::GetTypeByName(std::string typeRef)
{

  std::transform(typeRef.begin(), typeRef.end(), typeRef.begin(), tolower);

  if (typeRef == "int") {
    return VarValue::kType_Integer;
  }
  if (typeRef == "float") {
    return VarValue::kType_Float;
  }
  if (typeRef == "string") {
    return VarValue::kType_String;
  }
  if (typeRef == "bool") {
    return VarValue::kType_Bool;
  }
  if (typeRef == "identifier") {
    assert(0);
  }
  if (typeRef == "string[]") {
    return VarValue::kType_StringArray;
  }
  if (typeRef == "int[]") {
    return VarValue::kType_IntArray;
  }
  if (typeRef == "float[]") {
    return VarValue::kType_FloatArray;
  }
  if (typeRef == "bool[]") {
    return VarValue::kType_BoolArray;
  }
  if (typeRef.find("[]") != std::string::npos) {
    return VarValue::kType_ObjectArray;
  }
  if (typeRef == "none") {
    // assert(false);
    return VarValue::kType_Object;
  }
  return VarValue::kType_Object;
}

uint8_t ActivePexInstance::GetArrayElementType(uint8_t type)
{
  uint8_t returnType;

  switch (type) {
    case VarValue::kType_ObjectArray:
      returnType = VarValue::kType_Object;

      break;
    case VarValue::kType_StringArray:
      returnType = VarValue::kType_String;

      break;
    case VarValue::kType_IntArray:
      returnType = VarValue::kType_Integer;

      break;
    case VarValue::kType_FloatArray:
      returnType = VarValue::kType_Float;

      break;
    case VarValue::kType_BoolArray:
      returnType = VarValue::kType_Bool;

      break;
    default:
      spdlog::error("ActivePexInstance::GetArrayElementType - Unable to get "
                    "required type for {}",
                    static_cast<int>(type));
      returnType = VarValue::kType_Object;
      break;
  }

  return returnType;
}

uint8_t ActivePexInstance::GetArrayTypeByElementType(uint8_t type)
{
  uint8_t returnType;

  switch (type) {
    case VarValue::kType_Object:
      returnType = VarValue::kType_ObjectArray;

      break;
    case VarValue::kType_String:
      returnType = VarValue::kType_StringArray;

      break;
    case VarValue::kType_Integer:
      returnType = VarValue::kType_IntArray;

      break;
    case VarValue::kType_Float:
      returnType = VarValue::kType_FloatArray;

      break;
    case VarValue::kType_Bool:
      returnType = VarValue::kType_BoolArray;

      break;
    default:
      spdlog::error("ActivePexInstance::GetArrayTypeByElementType - Unable to "
                    "get required type for {}",
                    static_cast<int>(type));
      returnType = VarValue::kType_ObjectArray;
      break;
  }

  return returnType;
}

void ActivePexInstance::CastObjectToObject(const VirtualMachine& vm,
                                           VarValue* result,
                                           VarValue* scriptToCastOwner)
{
  static const VarValue kNone = VarValue::None();

  if (scriptToCastOwner->GetType() != VarValue::kType_Object) {
    *result = kNone;
    return spdlog::trace(
      "CastObjectToObject {} -> {} (object is not an object)",
      scriptToCastOwner->ToString(), result->ToString());
  }

  if (*scriptToCastOwner == kNone) {
    *result = kNone;
    return spdlog::trace("CastObjectToObject {} -> {} (object is None)",
                         scriptToCastOwner->ToString(), result->ToString());
  }

  const std::string& resultTypeName = result->objectType;

  VarValue tmp;
  std::vector<std::string> outClassesStack;

  if (tmp == kNone) {
    tmp = TryCastToBaseClass(vm, resultTypeName, scriptToCastOwner,
                             outClassesStack);
    if (tmp != kNone) {
      spdlog::trace("CastObjectToObject {} -> {} (base class found: {})",
                    scriptToCastOwner->ToString(), tmp.ToString(),
                    resultTypeName);
    }
  }

  if (tmp == kNone) {
    tmp = TryCastMultipleInheritance(vm, resultTypeName, scriptToCastOwner);
    if (tmp != kNone) {
      spdlog::trace(
        "CastObjectToObject {} -> {} (multiple inheritance found: {})",
        scriptToCastOwner->ToString(), tmp.ToString(), resultTypeName);
    }
  }

  if (tmp == kNone) {
    spdlog::trace(
      "CastObjectToObject {} -> {} (match not found, wanted {}, stack is {})",
      scriptToCastOwner->ToString(), tmp.ToString(), resultTypeName,
      fmt::join(outClassesStack, ", "));
  }

  *result = tmp;
}

VarValue ActivePexInstance::TryCastToBaseClass(
  const VirtualMachine& vm, const std::string& resultTypeName,
  VarValue* scriptToCastOwner, std::vector<std::string>& outClassesStack)
{
  auto object = static_cast<IGameObject*>(*scriptToCastOwner);
  if (!object) {
    return VarValue::None();
  }

  std::string scriptName = object->GetParentNativeScript();
  outClassesStack.push_back(scriptName);
  std::set<CIString> visited;
  while (true) {
    if (scriptName.empty()) {
      break;
    }

    if (vm.IsStrictExecution() &&
        (!visited.insert(CIString{ scriptName.begin(), scriptName.end() })
            .second ||
         visited.size() > 128))
      throw std::runtime_error("Papyrus cast class cycle/depth exceeded");

    if (!Utils::stricmp(resultTypeName.data(), scriptName.data())) {
      return *scriptToCastOwner;
    }

    // TODO: Test this with attention
    // Here is the case when i.e. variable with type 'Form' casts to
    // 'ObjectReference' while it's actually an Actor

    auto myScriptPex = vm.GetPexByName(scriptName);

    if (!myScriptPex.fn) {
      spdlog::error("Script not found: {}", scriptName);
      break;
    }

    scriptName = myScriptPex.fn()->objectTable[0].parentClassName;
    outClassesStack.push_back(scriptName);
  }

  return VarValue::None();
}

VarValue ActivePexInstance::TryCastMultipleInheritance(
  const VirtualMachine& vm, const std::string& resultTypeName,
  VarValue* scriptToCastOwner)
{
  auto object = static_cast<IGameObject*>(*scriptToCastOwner);
  if (!object) {
    return VarValue::None();
  }

  // TODO: support cast to base class in parallel inheritance chain
  // i.e. X extends Y, in script Z we cast smth to X while it is Y
  // Current code would fail in this case. I guess it'd be better to find such
  // cases in game, then implement.
  if (vm.IsStrictExecution()) {
    size_t matches = 0;
    for (const auto& root : object->ListActivePexInstances()) {
      for (auto entry = root; entry && entry->IsValid();
           entry = entry->GetParentInstance()) {
        if (!Utils::stricmp(entry->GetSourcePexName().c_str(),
                            resultTypeName.c_str())) {
          if (++matches > 1)
            throw std::runtime_error("Ambiguous Papyrus script cast");
          break;
        }
      }
    }
    return matches ? *scriptToCastOwner : VarValue::None();
  }
  for (auto& script : object->ListActivePexInstances()) {
    if (Utils::stricmp(script->GetSourcePexName().data(),
                       resultTypeName.data()) == 0) {
      return script->activeInstanceOwner;
    }
  }

  return VarValue::None();
}

VarValue& ActivePexInstance::ResetNoneVarAndReturn()
{
  // You can't just initialize noneVar once because it can be modified outside
  // unexpectedly
  noneVar = VarValue::None();
  return noneVar;
}

bool ActivePexInstance::HasParent(ActivePexInstance* script,
                                  std::string castToTypeName)
{

  if (script != nullptr) {

    if (script->sourcePex.fn()->source == castToTypeName)
      return true;

    if (script->parentInstance != nullptr &&
        script->parentInstance->sourcePex.fn()->source != "") {
      if (script->parentInstance->sourcePex.fn()->source == castToTypeName)
        return true;
      else
        return HasParent(script->parentInstance.get(), castToTypeName);
    }
  }

  return false;
}

bool ActivePexInstance::HasChild(ActivePexInstance* script,
                                 std::string castToTypeName)
{

  if (script != nullptr) {

    if (script->sourcePex.fn()->source == castToTypeName)
      return true;

    if (script->childrenName != "") {
      if (script->childrenName == castToTypeName)
        return true;
    }
  }
  return false;
}

// TODO: optimize "name" to be passed by a const char * instead of std::string
VarValue& ActivePexInstance::GetVariableValueByName(std::vector<Local>* locals,
                                                    std::string name)
{
  if (name == "self" ||
      (parentVM->IsStrictExecution() &&
       !Utils::stricmp(name.c_str(), "self"))) {
    return activeInstanceOwner;
  }

  if (parentVM->IsStrictExecution() &&
      !Utils::stricmp(name.c_str(), "::State")) {
    const auto root = GetAttachedRoot();
    if (!root->variables)
      throw std::runtime_error("Missing Papyrus script variables");
    if (auto value =
          root->variables->GetVariableByName("::State", *root->sourcePex.fn()))
      return *value;
    throw std::runtime_error("Missing Papyrus instance state variable");
  }

  if (locals)
    for (auto& var : *locals) {
      if (var.first == name) {
        return var.second;
      }
    }

  try {
    if (variables)
      if (auto var =
            variables->GetVariableByName(name.data(), *sourcePex.fn()))
        return *var;
  } catch (std::exception& e) {
    if (parentVM->IsStrictExecution())
      throw;
    spdlog::error("ActivePexInstance::GetVariableValueByName - "
                  "GetVariableByName errored with '{}'",
                  e.what());
    return ResetNoneVarAndReturn();
  } catch (...) {
    if (parentVM->IsStrictExecution())
      throw;
    spdlog::critical("ActivePexInstance::GetVariableValueByName - "
                     "GetVariableByName errored with unknown error");
    std::terminate();
    return ResetNoneVarAndReturn();
  }

  if (parentVM->IsStrictExecution())
    throw std::runtime_error("Unknown script variable: " + name);
  for (auto& _name : identifiersValueNameCache) {
    if ((const char*)(*_name) == name) {
      return *_name;
    }
  }

  if (parentVM->IsNativeFunctionByNameExisted(GetSourcePexName())) {
    auto functionName = std::make_shared<VarValue>(name);

    identifiersValueNameCache.push_back(functionName);
    return *identifiersValueNameCache.back();
  }

  for (auto& _string : sourcePex.fn()->stringTable.GetStorage()) {
    if (_string == name) {
      auto stringTableValue = std::make_shared<VarValue>(name);

      identifiersValueNameCache.push_back(stringTableValue);
      return *identifiersValueNameCache.back();
    }
  }

  for (auto& _string :
       parentInstance->sourcePex.fn()->stringTable.GetStorage()) {
    if (_string == name) {
      auto stringTableParentValue = std::make_shared<VarValue>(name);

      identifiersValueNameCache.push_back(stringTableParentValue);
      return *identifiersValueNameCache.back();
    }
  }

  spdlog::error("ActivePexInstance::GetVariableValueByName - Failed all "
                "attempts to find variable '{}'",
                name);
  return ResetNoneVarAndReturn();
}
