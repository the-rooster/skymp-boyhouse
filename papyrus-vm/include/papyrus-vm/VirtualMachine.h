#pragma once
#include "CIString.h"
#include "Structures.h"
#include "Continuation.h"
#include <MakeID.h>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

using NativeFunction =
  std::function<VarValue(VarValue self, std::vector<VarValue> arguments)>;

class VirtualMachine;

class StackIdHolder
{
public:
  StackIdHolder(VirtualMachine& vm_);
  ~StackIdHolder();
  int32_t GetStackId() const;

  StackIdHolder& operator=(const StackIdHolder&) = delete;
  StackIdHolder(const StackIdHolder&) = delete;

private:
  int32_t stackId;
  std::shared_ptr<MakeID> makeId;
};

class StackDepthHolder
{
public:
  StackDepthHolder();

  size_t GetStackDepth() const;
  void IncreaseStackDepth();
  void DecreaseStackDepth();

  StackDepthHolder& operator=(const StackDepthHolder&) = delete;
  StackDepthHolder(const StackDepthHolder&) = delete;

private:
  size_t depth = 0;
};

struct StackData
{
  StackIdHolder stackIdHolder;
};

struct VmExceptionInfo
{
  std::string what;
  std::string sourcePex;
};

class VirtualMachine
{
  friend class StackIdHolder;
  friend class ActivePexInstance;

public:
  using OnEnter = std::function<void(const StackData&)>;
  using MissingScriptHandler =
    std::function<std::optional<PexScript::Lazy>(std::string)>;

  struct ScriptInfo
  {
    std::string name;
    std::shared_ptr<IVariablesHolder> vars;
  };

  VirtualMachine(const std::vector<PexScript::Lazy>& loadedScripts);
  VirtualMachine(const std::vector<std::shared_ptr<PexScript>>& loadedScripts);

  void SetMissingScriptHandler(const MissingScriptHandler& handler);
  void SetStrictExecution(bool enabled)
  {
    strictExecution = enabled;
    executedInstructions = 0;
  }
  bool IsStrictExecution() const { return strictExecution; }
  void CheckExecutionBudget();
  uint32_t GetExecutedInstructions() const noexcept;
  // Restore cumulative work when a host job reconstructs a fresh VM. This
  // never lowers the current counter or makes an exhausted VM runnable again.
  void RestoreExecutionBudget(uint32_t executed);

  // Only an explicitly registered host native may request a checkpoint, and
  // only inside StartResumableFunction/ResumeFunction. Ordinary promises keep
  // their existing strict-execution rejection. No timer is registered here.
  [[noreturn]] void RequestSuspension(const std::string& token,
                                    const std::string& resultType);

  void AddObject(std::shared_ptr<IGameObject> self,
                 const std::vector<ScriptInfo>& scripts);

  void RemoveObject(std::shared_ptr<IGameObject> self); // ?

  void RegisterFunction(const std::string& className,
                        const std::string& functionName,
                        const FunctionType& type, const NativeFunction& fn);

  void SendEvent(std::shared_ptr<IGameObject> self, const char* eventName,
                 const std::vector<VarValue>& arguments,
                 OnEnter enter = nullptr);

  void SendEvent(ActivePexInstance* instance, const char* eventName,
                 const std::vector<VarValue>& arguments);

  struct EventHandlerSelection
  {
    std::shared_ptr<ActivePexInstance> root, implementation;
    // The selected implementation's state, empty when using its fallback.
    // The attached root remains distinct from an inherited implementation.
    std::string state, function;
    FunctionInfo body;
  };
  std::vector<std::shared_ptr<ActivePexInstance>> GetEventAttachments(
    const std::shared_ptr<IGameObject>& self) const;
  // Strict selection only; does not invoke the handler. Select each attachment
  // when its turn begins, then retain this identity across any suspension.
  std::optional<EventHandlerSelection> SelectEventHandler(
    const std::shared_ptr<ActivePexInstance>& root, const char* eventName);

  // Campaign transactions share one stack context/budget and must complete
  // every handler synchronously before publishing any external effect.
  void SendEventSynchronous(std::shared_ptr<IGameObject> self,
                            const char* eventName,
                            const std::vector<VarValue>& arguments,
                            std::shared_ptr<StackData> stackData);

  VarValue CallMethod(IGameObject* self, const char* methodName,
                      std::vector<VarValue>& arguments,
                      std::shared_ptr<StackData> stackData = nullptr,
                      const std::vector<std::shared_ptr<ActivePexInstance>>*
                        activePexInstancesOverride = nullptr,
                      const char* receiverType = nullptr);

  VarValue CallStatic(const std::string& className,
                      const std::string& functionName,
                      std::vector<VarValue>& arguments,
                      std::shared_ptr<StackData> stackData = nullptr);

  PexScript::Lazy GetPexByName(const std::string& name) const;

  std::shared_ptr<ActivePexInstance> CreateActivePexInstance(
    const std::string& pexScriptName, VarValue activeInstanceOwner,
    const std::shared_ptr<IVariablesHolder>& mapForFillProperties,
    const std::string& childrenName);

  bool IsNativeFunctionByNameExisted(const std::string& name) const;

  std::set<CIString> ListClasses() const;
  CIString GetBaseClass(const CIString& className) const;
  std::set<CIString> ListStaticFunctions(const CIString& className) const;
  std::set<CIString> ListMethods(const CIString& className) const;
  NativeFunction GetFunctionImplementation(const CIString& className,
                                           const CIString& functionName,
                                           bool isStatic) const;
  bool DynamicCast(const VarValue& object, const CIString& className) const;

private:
  struct SuspensionRequest : std::runtime_error
  {
    std::string token, resultType;
    SuspensionRequest(std::string token_, std::string resultType_)
      : std::runtime_error("Explicit Papyrus suspension")
      , token(std::move(token_))
      , resultType(std::move(resultType_))
    {
    }
  };
  size_t activeFrames = 0;
  size_t checkpointLocals = 0;
  std::optional<PapyrusContinuation> checkpointFrame;
  bool checkpointSuspended = false;
  std::string suspendedReturnType;
  bool strictExecution = false;
  size_t executedInstructions = 0;
  CIMap<PexScript::Lazy> allLoadedScripts;

  CIMap<CIMap<NativeFunction>> nativeFunctions, nativeStaticFunctions;

  std::map<std::string, std::shared_ptr<ActivePexInstance>>
    instancesForStaticCalls;

  std::set<std::shared_ptr<IGameObject>> gameObjectsHolder;

  MissingScriptHandler missingScriptHandler;

  std::shared_ptr<MakeID> stackIdMaker;
};
