#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FunctionCode.h"
#include "Continuation.h"
#include "FunctionInfo.h"
#include "IVariablesHolder.h"
#include "PexScript.h"
#include "Promise.h"
#include "VarValue.h"

class VirtualMachine;
class StackData;

class ActivePexInstance
{
  friend class VirtualMachine;

public:
  using Local = std::pair<std::string, VarValue>;

  ActivePexInstance();
  ActivePexInstance(
    PexScript::Lazy sourcePex,
    const std::shared_ptr<IVariablesHolder>& mapForFillProperties,
    VirtualMachine* parentVM, VarValue activeInstanceOwner,
    std::string childrenName);

  // TODO(#xyz): return const&
  FunctionInfo GetFunctionByName(const char* name,
                                 std::string stateName) const;

  VarValue& GetVariableValueByName(std::vector<Local>* optional,
                                   std::string name);

  VarValue& GetIndentifierValue(std::vector<Local>& locals, VarValue& value,
                                bool treatStringsAsIdentifiers = false);

  VarValue StartFunction(const FunctionInfo& function,
                         std::vector<VarValue>& arguments,
                         std::shared_ptr<StackData> stackData,
                         const PapyrusFunctionIdentity& identity = {});

  // Explicit host entry points for an attached function and its callers. They
  // do not enable latent quest stages/events or persist the surrounding job.
  PapyrusExecutionResult StartResumableFunction(
    const std::string& scope, const std::string& state,
    const std::string& function, std::vector<VarValue> arguments,
    std::shared_ptr<StackData> stackData);
  PapyrusExecutionResult ResumeFunction(
    const std::string& scope, const PapyrusContinuation& continuation,
    const VarValue& completion, std::shared_ptr<StackData> stackData);

  static uint8_t GetTypeByName(std::string typeRef);
  std::string GetActiveStateName() const;

  bool IsValid() const { return _IsValid; };

  const std::string& GetSourcePexName() const;

  const std::shared_ptr<ActivePexInstance> GetParentInstance() const
  {
    return parentInstance;
  };

  static uint8_t GetArrayElementType(uint8_t type);
  static uint8_t GetArrayTypeByElementType(uint8_t type);

  static void CastObjectToObject(const VirtualMachine& vm, VarValue* result,
                                 VarValue* scriptToCastOwner);

private:
  struct ExecutionContext;

  std::vector<std::pair<uint8_t, std::vector<VarValue*>>>
  TransformInstructions(std::vector<FunctionCode::Instruction>& sourceOpCode,
                        std::shared_ptr<std::vector<Local>> locals);

  std::shared_ptr<std::vector<ActivePexInstance::Local>> MakeLocals(
    const FunctionInfo& function, const std::vector<VarValue>& arguments);

  VarValue ExecuteAll(
    ExecutionContext& ctx,
    std::optional<VarValue> previousCallResult = std::nullopt);

  void ExecuteOpCode(ExecutionContext* ctx, uint8_t op,
                     const std::vector<VarValue*>& arguments);

  void ExecuteStrictProperty(ExecutionContext* ctx, uint8_t op,
                             const std::vector<VarValue*>& arguments);

  std::shared_ptr<ActivePexInstance> GetAttachedRoot() const;
  PapyrusFrame DescribeFrame(const PapyrusFunctionIdentity& identity) const;
  FunctionInfo FrameFunction(const PapyrusFunctionIdentity& identity) const;
  void ValidateResumableFunction(
    const FunctionInfo& function, const std::vector<VarValue>& arguments,
    const PapyrusFunctionIdentity& identity) const;
  void ValidateCheckpointEntry(const std::string& scope) const;
  void ValidateCallResult(const FunctionCode::Instruction& call,
                          std::vector<Local>& locals,
                          const VarValue& completion);
  std::shared_ptr<ActivePexInstance> ResolveFrame(const PapyrusFrame& frame);
  void ValidateFrameValue(const VarValue& value, const std::string& type,
                          bool discard = false) const;

  bool EnsureCallResultIsSynchronous(const VarValue& callResult,
                                     ExecutionContext* ctx);

  Object::PropInfo* GetProperty(const ActivePexInstance& scriptInstance,
                                std::string nameProperty, uint8_t flag);

  bool HasParent(ActivePexInstance* script, std::string castToTypeName);
  bool HasChild(ActivePexInstance* script, std::string castToTypeName);

  std::shared_ptr<ActivePexInstance> FillParentInstance(
    std::string nameNeedScript, VarValue activeInstanceOwner,
    const std::shared_ptr<IVariablesHolder>& mapForFillProperties);

  static VarValue TryCastToBaseClass(
    const VirtualMachine& vm, const std::string& resultTypeName,
    VarValue* scriptToCastOwner, std::vector<std::string>& outClassesStack);

  static VarValue TryCastMultipleInheritance(const VirtualMachine& vm,
                                             const std::string& resultTypeName,
                                             VarValue* scriptToCastOwner);

  VarValue& ResetNoneVarAndReturn();

  bool _IsValid = false;

  std::string childrenName;

  PexScript::Lazy sourcePex;
  VirtualMachine* parentVM = nullptr;

  VarValue activeInstanceOwner = VarValue::None();

  std::shared_ptr<ActivePexInstance> parentInstance;

  std::shared_ptr<IVariablesHolder> variables;
  std::vector<std::shared_ptr<VarValue>> identifiersValueNameCache;

  uint64_t promiseIdx = 0;
  std::map<uint64_t, std::shared_ptr<Viet::Promise<VarValue>>> promises;

  VarValue noneVar = VarValue::None();
};
