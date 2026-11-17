#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ScriptVariablesHolder.h"
#include "papyrus-vm/Reader.h"
#include "papyrus-vm/VirtualMachine.h"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <list>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
std::shared_ptr<VirtualMachine> CreateVirtualMachine()
{
  std::vector<std::string> allPath;
  std::vector<fs::path> pexFiles;

  if (!std::filesystem::exists(BUILT_PEX_DIR)) {
    throw std::runtime_error(
      "It seems this machine didn't compile .psc files");
  }

  fs::directory_iterator begin(BUILT_PEX_DIR);
  fs::directory_iterator end;

  std::copy_if(
    begin, end, std::back_inserter(pexFiles), [](const fs::path& path) {
      return fs::is_regular_file(path) && (path.extension() == ".pex");
    });

  for (auto file : pexFiles) {
    allPath.push_back(fs::absolute(file).generic_string());
  }

  REQUIRE(allPath.size() != 0);

  Reader reader(allPath);

  std::vector<std::shared_ptr<PexScript>> vector =
    reader.GetSourceStructures();

  auto vm = std::make_shared<VirtualMachine>(vector);

  std::shared_ptr<int> assertId = std::make_shared<int>(1);
  std::shared_ptr<std::stringstream> ss =
    std::make_shared<std::stringstream>();

  vm->RegisterFunction("", "Print", FunctionType::GlobalFunction,
                       [=](VarValue self, const std::vector<VarValue> args) {
                         if (args.size() >= 1) {
                           std::string showString =
                             static_cast<const char*>(args[0]);
                           *ss << std::endl
                               << "[!] Papyrus says: " << showString
                               << std::endl
                               << std::endl;
                           *assertId = 1;
                         }
                         return VarValue::None();
                       });

  vm->RegisterFunction("", "Assert", FunctionType::GlobalFunction,
                       [=](VarValue self, std::vector<VarValue> args) {
                         if (args.size() >= 1) {
                           bool success = static_cast<bool>(args[0]);
                           std::string message = "\t Assertion " +
                             std::string(success ? "succeed" : "failed") +
                             " (" + std::to_string(*assertId) + ")";
                           ++*assertId;
                           if (!success) {
                             std::cout << ss->str();
                             throw std::runtime_error(message);
                           }
                           *ss << message << std::endl;
                         }
                         return VarValue::None();
                       });
  vm->RegisterFunction("OpcodesTest", "TestFunction",
                       FunctionType::GlobalFunction,
                       [=](VarValue self, std::vector<VarValue> args) {
                         return VarValue(42); // random integer
                       });

  return vm;
}

class TestObject : public IGameObject
{
public:
  std::string myId = "0x006AFF2E";
  std::vector<std::shared_ptr<ActivePexInstance>> scripts;

  const char* GetStringID() override { return myId.c_str(); };

  const std::vector<std::shared_ptr<ActivePexInstance>>&
  ListActivePexInstances() const override
  {
    return scripts;
  }

  void AddScript(std::shared_ptr<ActivePexInstance> sctipt) noexcept override
  {
    scripts.push_back(sctipt);
  }
};

class MyScriptVariablesHolder : public ScriptVariablesHolder
{
public:
  MyScriptVariablesHolder(const char* scriptName)
    : ScriptVariablesHolder(scriptName, {}, {}, nullptr, nullptr, nullptr)
  {
    testObject.reset(new TestObject);
    var = VarValue(testObject.get());
  }

  VarValue* GetVariableByName(const char* name, const PexScript& pex) override
  {
    auto res = ScriptVariablesHolder::GetVariableByName(name, pex);
    if (name == std::string("::OpcodeRef_var")) {
      return &var;
    }
    return res;
  }

  std::shared_ptr<IGameObject> testObject;
  VarValue var;
};

}

TEST_CASE("Real pex parsing and execution", "[VirtualMachine]")
{
  auto vm = CreateVirtualMachine();
  if (!vm)
    return;

  std::vector<VirtualMachine::ScriptInfo> scripts;
  scripts.push_back({ "AAATestObject",
                      std::shared_ptr<ScriptVariablesHolder>(
                        new MyScriptVariablesHolder("AAATestObject")) });
  auto holder = std::shared_ptr<MyScriptVariablesHolder>(
    new MyScriptVariablesHolder("OpcodesTest"));
  scripts.push_back({ "OpcodesTest", holder });
  vm->AddObject(holder->testObject, scripts);

  std::vector<VarValue> functionArgs;
  vm->SendEvent(holder->testObject, "Main", functionArgs);

  //
  // Simple Latent test
  //

  auto latentHolder = std::make_shared<MyScriptVariablesHolder>("LatentTest");
  vm->AddObject(latentHolder->testObject, { { "LatentTest", latentHolder } });

  int nonLatentCalls = 0;

  vm->RegisterFunction("LatentTest", "NonLatentFunc",
                       FunctionType::GlobalFunction,
                       [&](VarValue self, std::vector<VarValue> args) {
                         nonLatentCalls++;
                         return VarValue::None();
                       });

  Viet::Promise<VarValue> promise;

  vm->RegisterFunction("LatentTest", "LatentFunc",
                       FunctionType::GlobalFunction,
                       [promise](VarValue self, std::vector<VarValue> args) {
                         return VarValue(promise);
                       });

  std::vector<VarValue> argsMain;
  vm->CallStatic("LatentTest", "Main", argsMain);

  REQUIRE(nonLatentCalls == 1);

  promise.Resolve(VarValue::None());

  REQUIRE(nonLatentCalls == 2);

  //
  // Return Value Latent test
  //

  auto result = VarValue::None();

  Viet::Promise<VarValue> pr;
  std::vector<VarValue> argsLatentAdd;

  vm->RegisterFunction("LatentTest", "LatentAdd", FunctionType::GlobalFunction,
                       [&](VarValue self, std::vector<VarValue> args) {
                         argsLatentAdd = args;
                         return VarValue(pr);
                       });

  std::vector<VarValue> argsMain2;
  vm->CallStatic("LatentTest", "Main2", argsMain2).Then([&](VarValue v) {
    result = v;
  });

  REQUIRE(result == VarValue::None());
  pr.Resolve(argsLatentAdd[0] + argsLatentAdd[1]);
  REQUIRE(result == VarValue(5));
}

TEST_CASE("Test nested latent calls", "[VirtualMachine]")
{
  auto vm = CreateVirtualMachine();
  if (!vm)
    return;

  struct DoubleTask
  {
    int arg = 0;
    Viet::Promise<VarValue> pr;
  };
  std::list<DoubleTask> t;

  vm->RegisterFunction(
    "LatentTest", "LatentDouble", FunctionType::GlobalFunction,
    [&](VarValue self, const std::vector<VarValue>& args) {
      Viet::Promise<VarValue> pr;
      t.push_back({ static_cast<int>(args[0].CastToInt()), pr });
      return VarValue(pr);
    });

  VarValue result;
  std::vector<VarValue> args;
  vm->CallStatic("LatentTest", "Main3", args).Then([&](VarValue v) {
    result = v;
  });

  REQUIRE(result == VarValue::None());

  while (t.size() > 0) {
    t.front().pr.Resolve(VarValue(t.front().arg * 2));
    t.pop_front();
  }

  REQUIRE(result == VarValue(6));
}

namespace {
class EventVariables : public IVariablesHolder
{
public:
  VarValue state = VarValue("Ready");

  VarValue* GetVariableByName(const char* name, const PexScript&) override
  {
    return std::string(name) == "::State" ? &state : nullptr;
  }
};

VarValue EventName(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}

FunctionInfo EventBody(int marker)
{
  FunctionInfo function;
  function.valid = true;
  function.returnType = "None";
  function.locals = { { "temp", "None" } };
  function.code.instructions = {
    { FunctionCode::kOp_CallStatic,
      { VarValue("EventFixture"), VarValue("Record"), EventName("temp"),
        VarValue(1), VarValue(marker) } },
    { FunctionCode::kOp_Return, { VarValue::None() } }
  };
  return function;
}

std::shared_ptr<PexScript> EventScript(
  const std::string& name, const std::string& parent,
  const std::vector<Object::StateInfo>& states)
{
  auto script = std::make_shared<PexScript>();
  script->source = name;
  Object object;
  object.NameIndex = name;
  object.parentClassName = parent;
  object.states = states;
  script->objectTable.push_back(std::move(object));
  return script;
}

std::shared_ptr<StackData> EventStack(VirtualMachine& vm)
{
  return std::shared_ptr<StackData>(new StackData{ StackIdHolder(vm) });
}
}

TEST_CASE(
  "Strict event selection preserves attachment and per-level state fallback",
  "[VirtualMachine][EventSelection]")
{
  auto parent = EventScript(
    "EventParent", "",
    { { "Ready",
        { { "OnInherited", EventBody(1) }, { "OnShadow", EventBody(2) } } },
      { "", { { "OnFallback", EventBody(3) } } } });
  auto child = EventScript("EventChild", "EventParent",
                           { { "Ready", { { "OnCurrent", EventBody(4) } } },
                             { "", { { "OnShadow", EventBody(5) } } } });
  VirtualMachine vm(std::vector<std::shared_ptr<PexScript>>{ child, parent });
  vm.SetStrictExecution(true);
  auto object = std::make_shared<TestObject>();
  auto variables = std::make_shared<EventVariables>();
  vm.AddObject(object, { { "EventChild", variables } });
  const auto root = object->scripts.at(0);

  const auto inherited = vm.SelectEventHandler(root, "OnInherited");
  REQUIRE(inherited);
  REQUIRE(inherited->root == root);
  REQUIRE(inherited->implementation == root->GetParentInstance());
  REQUIRE(inherited->state == "Ready");
  REQUIRE(inherited->function == "OnInherited");
  REQUIRE(inherited->body.valid);

  const auto fallback = vm.SelectEventHandler(root, "OnFallback");
  REQUIRE(fallback);
  REQUIRE(fallback->root == root);
  REQUIRE(fallback->implementation == root->GetParentInstance());
  REQUIRE(fallback->state.empty());

  // A child's empty-state handler wins over the parent's active-state body.
  const auto shadow = vm.SelectEventHandler(root, "OnShadow");
  REQUIRE(shadow);
  REQUIRE(shadow->implementation == root);
  REQUIRE(shadow->state.empty());
  const auto current = vm.SelectEventHandler(root, "OnCurrent");
  REQUIRE(current);
  REQUIRE(current->implementation == root);
  REQUIRE(current->state == "Ready");
  REQUIRE_FALSE(vm.SelectEventHandler(root, "OnAbsent"));
}

TEST_CASE(
  "An inherited event selection resumes its chosen state after a state change",
  "[VirtualMachine][EventSelection][PapyrusContinuation]")
{
  auto body = EventBody(7);
  body.code.instructions.insert(
    body.code.instructions.begin(),
    { FunctionCode::kOp_CallStatic,
      { VarValue("EventFixture"), VarValue("Await"), EventName("temp"),
        VarValue(0) } });
  auto parent = EventScript("WaitingEventParent", "",
                            { { "Ready", { { "OnSignal", body } } } });
  auto child = EventScript("WaitingEventChild", "WaitingEventParent", {});
  VirtualMachine vm(std::vector<std::shared_ptr<PexScript>>{ child, parent });
  vm.SetStrictExecution(true);
  auto object = std::make_shared<TestObject>();
  auto variables = std::make_shared<EventVariables>();
  vm.AddObject(object, { { "WaitingEventChild", variables } });
  std::vector<int> effects;
  vm.RegisterFunction("EventFixture", "Record", FunctionType::GlobalFunction,
                      [&](VarValue, std::vector<VarValue> args) {
                        effects.push_back(static_cast<int32_t>(args.at(0)));
                        return VarValue::None();
                      });
  vm.RegisterFunction("EventFixture", "Await", FunctionType::GlobalFunction,
                      [&](VarValue, std::vector<VarValue>) -> VarValue {
                        vm.RequestSuspension("event-operation", "None");
                      });
  const auto selected =
    vm.SelectEventHandler(object->scripts.at(0), "OnSignal");
  REQUIRE(selected);
  const auto stack = EventStack(vm);
  const auto started = selected->implementation->StartResumableFunction(
    "event-scope", selected->state, selected->function, {}, stack);
  REQUIRE(started.continuation);
  REQUIRE(started.continuation->rootScript == "WaitingEventChild");
  REQUIRE(started.continuation->script == "WaitingEventParent");
  REQUIRE(started.continuation->state == "Ready");
  REQUIRE(effects.empty());

  variables->state = VarValue("Changed");
  REQUIRE_FALSE(vm.SelectEventHandler(object->scripts.at(0), "OnSignal"));
  const auto resumed = selected->implementation->ResumeFunction(
    "event-scope", *started.continuation, VarValue::None(), stack);
  REQUIRE_FALSE(resumed.continuation);
  REQUIRE(resumed.value == VarValue::None());
  REQUIRE(effects == std::vector<int>{ 7 });
}

TEST_CASE(
  "Synchronous events snapshot roots and select each state at delivery time",
  "[VirtualMachine][EventSelection]")
{
  auto first = EventScript("FirstEvent", "",
                           { { "", { { "OnSignal", EventBody(10) } } } });
  auto second =
    EventScript("SecondEvent", "",
                { { "Ready", { { "OnSignal", EventBody(20) } } },
                  { "Changed", { { "OnSignal", EventBody(30) } } } });
  auto late = EventScript("LateEvent", "",
                          { { "", { { "OnSignal", EventBody(40) } } } });
  VirtualMachine vm(
    std::vector<std::shared_ptr<PexScript>>{ first, second, late });
  vm.SetStrictExecution(true);
  auto object = std::make_shared<TestObject>();
  auto firstVariables = std::make_shared<EventVariables>();
  auto secondVariables = std::make_shared<EventVariables>();
  auto lateVariables = std::make_shared<EventVariables>();
  vm.AddObject(
    object,
    { { "FirstEvent", firstVariables }, { "SecondEvent", secondVariables } });
  const auto before = vm.SelectEventHandler(object->scripts.at(1), "OnSignal");
  REQUIRE(before);
  REQUIRE(before->state == "Ready");
  std::vector<int> effects;
  vm.RegisterFunction("EventFixture", "Record", FunctionType::GlobalFunction,
                      [&](VarValue, std::vector<VarValue> args) {
                        const auto marker = static_cast<int32_t>(args.at(0));
                        effects.push_back(marker);
                        if (marker == 10) {
                          secondVariables->state = VarValue("Changed");
                          vm.AddObject(object,
                                       { { "LateEvent", lateVariables } });
                        }
                        return VarValue::None();
                      });
  vm.SendEventSynchronous(object, "OnSignal", {}, EventStack(vm));
  REQUIRE(object->scripts.size() == 3);
  REQUIRE(effects == std::vector<int>{ 10, 30 });
  REQUIRE(before->state == "Ready");
}

TEST_CASE("Strict event selection rejects invalid entries and consumes the "
          "shared budget",
          "[VirtualMachine][EventSelection]")
{
  const auto flags = GENERATE(uint8_t(1), uint8_t(2));
  auto invalid = EventBody(1);
  invalid.flags = flags;
  auto parent = EventScript("InvalidEventParent", "", {});
  auto script = EventScript(
    "InvalidEvent", "InvalidEventParent",
    { { "Ready", { { "OnInvalid", invalid }, { "OnSignal", EventBody(2) } } },
      { "", { { "OnInvalid", EventBody(3) } } } });
  VirtualMachine vm(std::vector<std::shared_ptr<PexScript>>{ script, parent });
  auto object = std::make_shared<TestObject>();
  auto variables = std::make_shared<EventVariables>();
  vm.AddObject(object, { { "InvalidEvent", variables } });
  const auto root = object->scripts.at(0);
  REQUIRE_THROWS(vm.SelectEventHandler(root, "OnSignal"));
  vm.SetStrictExecution(true);
  REQUIRE_THROWS(vm.SelectEventHandler({}, "OnSignal"));
  REQUIRE_THROWS(vm.SelectEventHandler(root, nullptr));
  REQUIRE_THROWS(vm.SelectEventHandler(root, ""));
  REQUIRE_THROWS(vm.SelectEventHandler(root->GetParentInstance(), "OnSignal"));
  VirtualMachine foreign(std::vector<PexScript::Lazy>{});
  foreign.SetStrictExecution(true);
  REQUIRE_THROWS(foreign.SelectEventHandler(root, "OnSignal"));
  REQUIRE_THROWS(vm.SelectEventHandler(root, "OnInvalid"));
  REQUIRE(vm.GetExecutedInstructions() == 1);
  REQUIRE(vm.SelectEventHandler(root, "OnSignal"));
  REQUIRE(vm.GetExecutedInstructions() == 2);
  vm.RestoreExecutionBudget(100000);
  REQUIRE_THROWS(vm.SelectEventHandler(root, "OnSignal"));
}
