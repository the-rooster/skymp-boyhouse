#include "QuestVmState.h"
#include "papyrus-vm/ActivePexInstance.h"
#include "papyrus-vm/VirtualMachine.h"
#include <catch2/catch_all.hpp>
#include <map>

namespace {
using Json = nlohmann::json;
VarValue Name(const char* name)
{
  return VarValue(VarValue::kType_Identifier, name);
}
class CheckpointObject : public IGameObject
{
public:
  std::string id = "campaign:1/quest:2";
  std::vector<std::shared_ptr<ActivePexInstance>> scripts;
  const char* GetStringID() override { return id.c_str(); }
  const char* GetParentNativeScript() override { return "Quest"; }
  bool EqualsByValue(const IGameObject& other) const override
  {
    const auto* object = dynamic_cast<const CheckpointObject*>(&other);
    return object && object->id == id;
  }
  const std::vector<std::shared_ptr<ActivePexInstance>>&
  ListActivePexInstances() const override
  {
    return scripts;
  }
  void AddScript(std::shared_ptr<ActivePexInstance> script) noexcept override
  {
    scripts.push_back(std::move(script));
  }
};
class CheckpointVariables : public IVariablesHolder
{
public:
  QuestVmState& heap;
  std::map<std::string, std::map<std::string, VarValue>> values;
  explicit CheckpointVariables(QuestVmState& heap_)
    : heap(heap_)
  {
  }
  VarValue* GetVariableByName(const char* name,
                              const PexScript& script) override
  {
    auto& fields = values[script.source];
    if (fields.count(name))
      return &fields.at(name);
    std::string type;
    VarValue initial;
    if (std::string(name) == "::State") {
      type = "String";
      initial = VarValue(script.objectTable.at(0).autoStateName);
    } else {
      for (const auto& declaration : script.objectTable.at(0).variables)
        if (declaration.name == name) {
          type = declaration.typeName;
          initial = declaration.value;
        }
    }
    if (type.empty())
      throw std::runtime_error("Missing checkpoint fixture variable");
    const auto saved = heap.GetScript("quest/" + script.source);
    if (!saved.empty() && saved.at("values").contains(name))
      initial = heap.Decode(saved.at("values").at(name));
    initial.objectType = type;
    return &fields.emplace(name, initial).first->second;
  }
  void Save()
  {
    for (const auto& [script, fields] : values) {
      auto encoded = Json::object();
      for (const auto& [name, value] : fields)
        encoded[name] = heap.Encode(value);
      heap.SetScript("quest/" + script,
                     { { "schemaVersion", 1 }, { "values", encoded } });
    }
  }
};
std::shared_ptr<PexScript> Program()
{
  auto script = std::make_shared<PexScript>();
  script->source = "ResumeFixture";
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "Quest";
  object.variables = { { "reward", "Int", 0, VarValue(0) },
                       { "shared", "Int[]", 0, VarValue::None() } };
  FunctionInfo function;
  function.valid = true;
  function.returnType = "Int";
  function.locals = { { "localOnly", "Int[]" },
                      { "sharedLocal", "Int[]" },
                      { "result", "Int" } };
  function.code.instructions = {
    { FunctionCode::kOp_IAdd,
      { Name("reward"), Name("reward"), VarValue(1) } },
    { FunctionCode::kOp_ArrayCreate, { Name("localOnly"), VarValue(1) } },
    { FunctionCode::kOp_ArraySetElement,
      { Name("localOnly"), VarValue(0), VarValue(7) } },
    { FunctionCode::kOp_Assign, { Name("sharedLocal"), Name("shared") } },
    { FunctionCode::kOp_CallStatic,
      { VarValue("CheckpointHost"), VarValue("Await"), Name("result"),
        VarValue(1), VarValue("first") } },
    { FunctionCode::kOp_ArraySetElement,
      { Name("sharedLocal"), VarValue(0), Name("result") } },
    { FunctionCode::kOp_ArrayGetElement,
      { Name("result"), Name("localOnly"), VarValue(0) } },
    { FunctionCode::kOp_IAdd,
      { Name("reward"), Name("reward"), Name("result") } },
    { FunctionCode::kOp_CallStatic,
      { VarValue("CheckpointHost"), VarValue("Await"), Name("result"),
        VarValue(1), VarValue("second") } },
    { FunctionCode::kOp_IAdd,
      { Name("reward"), Name("reward"), Name("result") } },
    { FunctionCode::kOp_Return, { Name("reward") } }
  };
  object.states.push_back({ "", { { "Run", function } } });
  script->objectTable.push_back(std::move(object));
  return script;
}
struct CheckpointHost
{
  std::shared_ptr<PexScript> script = Program();
  std::shared_ptr<PexScript> base = std::make_shared<PexScript>();
  std::shared_ptr<CheckpointObject> object =
    std::make_shared<CheckpointObject>();
  QuestVmState heap;
  std::shared_ptr<CheckpointVariables> variables;
  std::unique_ptr<VirtualMachine> vm;
  size_t awaitCalls = 0;
  CheckpointHost(Json saved = Json::object())
    : heap(
        std::move(saved),
        [](IGameObject* object) -> Json { return object->GetStringID(); },
        [this](const Json& id) {
          if (id != object->id)
            throw std::runtime_error("Wrong campaign checkpoint object");
          return VarValue(object.get());
        })
  {
    base->source = "Quest";
    Object baseObject;
    baseObject.NameIndex = "Quest";
    base->objectTable.push_back(baseObject);
    variables = std::make_shared<CheckpointVariables>(heap);
    vm = std::make_unique<VirtualMachine>(
      std::vector<std::shared_ptr<PexScript>>{ script, base });
    vm->SetStrictExecution(true);
    vm->AddObject(object, { { script->source, variables } });
    vm->RegisterFunction(
      "CheckpointHost", "Await", FunctionType::GlobalFunction,
      [this](VarValue, std::vector<VarValue> args) -> VarValue {
        ++awaitCalls;
        vm->RequestSuspension(static_cast<const char*>(args.at(0)), "Int");
      });
    auto& shared = *variables->GetVariableByName("shared", *script);
    if (shared.GetType() == VarValue::kType_Object) {
      VarValue initial(static_cast<uint8_t>(VarValue::kType_IntArray));
      initial.pArray = std::make_shared<std::vector<VarValue>>(1, VarValue(9));
      shared = initial;
    }
  }
  std::shared_ptr<StackData> Stack()
  {
    return std::shared_ptr<StackData>(new StackData{ StackIdHolder{ *vm } });
  }
  PapyrusExecutionResult Start()
  {
    return object->scripts[0]->StartResumableFunction("campaign:1", "", "Run",
                                                      {}, Stack());
  }
  PapyrusExecutionResult Resume(const PapyrusContinuation& frame, int result)
  {
    return object->scripts[0]->ResumeFunction("campaign:1", frame,
                                              VarValue(result), Stack());
  }
  int Reward()
  {
    return static_cast<int>(*variables->GetVariableByName("reward", *script));
  }
  std::string Save(const PapyrusContinuation& frame)
  {
    variables->Save();
    heap.PutContinuation(frame);
    return heap.Save().dump();
  }
};
}

TEST_CASE("Papyrus leaf checkpoints survive two VM destructions without "
          "replaying preceding effects or losing local arrays",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  std::string saved;
  std::weak_ptr<IGameObject> oldObject;
  {
    CheckpointHost first;
    oldObject = first.object;
    const auto result = first.Start();
    REQUIRE(result.continuation);
    REQUIRE(result.continuation->instruction == 4);
    REQUIRE(first.Reward() == 1);
    REQUIRE(first.awaitCalls == 1);
    saved = first.Save(*result.continuation);
    REQUIRE(Json::parse(saved).at("arrays").size() == 2);
  }
  REQUIRE(oldObject.expired());
  {
    CheckpointHost second(Json::parse(saved));
    oldObject = second.object;
    const auto frame = second.heap.TakeContinuation("campaign:1", "first");
    REQUIRE_THROWS(second.heap.TakeContinuation("campaign:1", "first"));
    const auto& shared =
      *second.variables->GetVariableByName("shared", *second.script);
    REQUIRE(frame.locals[1].second.pArray == shared.pArray);
    REQUIRE(frame.locals[0].second.pArray != shared.pArray);
    const auto result = second.Resume(frame, 12);
    REQUIRE(result.continuation);
    REQUIRE(result.continuation->token == "second");
    REQUIRE(result.continuation->instruction == 8);
    REQUIRE(result.continuation->executedInstructions >
            frame.executedInstructions);
    REQUIRE(second.Reward() == 8);
    REQUIRE(static_cast<int>(shared.pArray->at(0)) == 12);
    REQUIRE(second.awaitCalls == 1);
    saved = second.Save(*result.continuation);
  }
  REQUIRE(oldObject.expired());
  CheckpointHost third(Json::parse(saved));
  const auto frame = third.heap.TakeContinuation("campaign:1", "second");
  const auto result = third.Resume(frame, 20);
  REQUIRE_FALSE(result.continuation);
  REQUIRE(static_cast<int>(result.value) == 28);
  REQUIRE(third.Reward() == 28);
  REQUIRE(third.awaitCalls == 0);
  third.variables->Save();
  const auto final = third.heap.Save();
  REQUIRE(final.at("continuations").empty());
  REQUIRE(final.at("arrays").size() == 1);
}

TEST_CASE("Papyrus continuation restore validates identities content and "
          "frame layout before executing more instructions",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  CheckpointHost host;
  auto frame = *host.Start().continuation;
  SECTION("scope")
  {
    frame.scope = "campaign:2";
  }
  SECTION("receiver")
  {
    frame.receiver = VarValue::None();
  }
  SECTION("attachment")
  {
    frame.rootScript = "OtherAttachment";
  }
  SECTION("implementation")
  {
    frame.script = "Quest";
  }
  SECTION("changed bytecode")
  {
    host.script->objectTable[0]
      .states[0]
      .functions[0]
      .function.code.instructions[0]
      .args[2] = VarValue(2);
  }
  SECTION("changed parent")
  {
    host.base->objectTable[0].autoStateName = "Changed";
  }
  SECTION("missing function")
  {
    frame.function = "Missing";
  }
  SECTION("not a call")
  {
    frame.instruction = 0;
  }
  SECTION("outside code")
  {
    frame.instruction = 100000;
  }
  SECTION("local name")
  {
    frame.locals[0].first = "wrong";
  }
  SECTION("declared local type")
  {
    frame.locals[0].second.objectType = "Float[]";
  }
  SECTION("local value type")
  {
    frame.locals[2].second = VarValue("wrong");
  }
  SECTION("missing local")
  {
    frame.locals.pop_back();
  }
  SECTION("untyped completion")
  {
    frame.resultType = "";
  }
  SECTION("wrong completion type")
  {
    frame.resultType = "Bool";
  }
  SECTION("empty token")
  {
    frame.token.clear();
  }
  SECTION("missing budget")
  {
    frame.executedInstructions = 0;
  }
  SECTION("exhausted budget")
  {
    frame.executedInstructions = 100001;
  }
  REQUIRE_THROWS(host.Resume(frame, 12));
  REQUIRE(host.Reward() == 1);
  REQUIRE(host.awaitCalls == 1);
}

TEST_CASE("Discarding a failed resumed candidate preserves the saved token "
          "and earlier effects for retry",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  auto configure = [](CheckpointHost& host, bool fail) {
    auto& code = host.script->objectTable[0]
                   .states[0]
                   .functions[0]
                   .function.code.instructions;
    code.insert(code.begin() + 6,
                { FunctionCode::kOp_CallStatic,
                  { VarValue("CheckpointHost"), VarValue("MaybeFail"),
                    Name("result"), VarValue(0) } });
    host.vm->RegisterFunction(
      "CheckpointHost", "MaybeFail", FunctionType::GlobalFunction,
      [fail](VarValue, std::vector<VarValue>) -> VarValue {
        if (fail)
          throw std::runtime_error("Resumed candidate failed");
        return VarValue(0);
      });
  };
  std::string committed;
  {
    CheckpointHost initial;
    configure(initial, false);
    committed = initial.Save(*initial.Start().continuation);
  }
  {
    CheckpointHost candidate(Json::parse(committed));
    configure(candidate, true);
    const auto frame = candidate.heap.TakeContinuation("campaign:1", "first");
    REQUIRE_THROWS_WITH(
      candidate.Resume(frame, 12),
      Catch::Matchers::ContainsSubstring("Resumed candidate failed"));
    const auto& shared =
      *candidate.variables->GetVariableByName("shared", *candidate.script);
    REQUIRE(static_cast<int>(shared.pArray->at(0)) == 12);
    REQUIRE_THROWS(candidate.heap.TakeContinuation("campaign:1", "first"));
    // This test host publishes only a successful candidate. Real quest jobs
    // must enforce the same boundary with effects and storage acknowledgement.
  }
  CheckpointHost retry(Json::parse(committed));
  configure(retry, false);
  REQUIRE(retry.Reward() == 1);
  const auto& shared =
    *retry.variables->GetVariableByName("shared", *retry.script);
  REQUIRE(static_cast<int>(shared.pArray->at(0)) == 9);
  const auto frame = retry.heap.TakeContinuation("campaign:1", "first");
  const auto result = retry.Resume(frame, 12);
  REQUIRE(result.continuation);
  REQUIRE(result.continuation->token == "second");
  REQUIRE(retry.Reward() == 8);
  REQUIRE(static_cast<int>(shared.pArray->at(0)) == 12);
}

TEST_CASE("Papyrus continuation cannot replenish its instruction budget",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  CheckpointHost host;
  SECTION("local result destination")
  {
  }
  SECTION("script-field result destination")
  {
    host.script->objectTable[0]
      .states[0]
      .functions[0]
      .function.code.instructions[4]
      .args[2] = Name("reward");
  }
  auto frame = *host.Start().continuation;
  frame.executedInstructions = 100000;
  REQUIRE_THROWS_WITH(host.Resume(frame, 12),
                      Catch::Matchers::ContainsSubstring("instruction quota"));
  REQUIRE(host.Reward() == 1);
  REQUIRE(host.awaitCalls == 1);
}

TEST_CASE("Leaf resume routes method and parent-native results to their "
          "original destinations",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  const bool parent = GENERATE(false, true);
  CheckpointHost host;
  host.vm->RegisterFunction(
    "Quest", "Await", FunctionType::Method,
    [&](VarValue self, std::vector<VarValue> args) -> VarValue {
      REQUIRE(static_cast<IGameObject*>(self) == host.object.get());
      REQUIRE(self.GetMetaStackId() >= 0);
      ++host.awaitCalls;
      host.vm->RequestSuspension(static_cast<const char*>(args.at(0)), "Int");
    });
  auto& call = host.script->objectTable[0]
                 .states[0]
                 .functions[0]
                 .function.code.instructions[4];
  call = parent
    ? FunctionCode::Instruction{ FunctionCode::kOp_CallParent,
                                 { VarValue("Await"), Name("result"),
                                   VarValue(1), VarValue("first") } }
    : FunctionCode::Instruction{ FunctionCode::kOp_CallMethod,
                                 { VarValue("Await"), Name("self"),
                                   Name("result"), VarValue(1),
                                   VarValue("first") } };
  const auto first = host.Start();
  REQUIRE(first.continuation);
  const auto next = host.Resume(*first.continuation, 17);
  REQUIRE(next.continuation);
  const auto& shared =
    *host.variables->GetVariableByName("shared", *host.script);
  REQUIRE(static_cast<int>(shared.pArray->at(0)) == 17);
  REQUIRE(host.Reward() == 8);
  REQUIRE(host.awaitCalls == 2);
}

TEST_CASE("Saved object parameters and typed None locals rebind to the new "
          "campaign object",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  auto configure = [](CheckpointHost& host) {
    auto& function =
      host.script->objectTable[0].states[0].functions[0].function;
    function.locals.push_back({ "maybe", "Quest" });
    function.params.push_back({ "target", "Quest" });
  };
  std::string saved;
  {
    CheckpointHost original;
    configure(original);
    auto result = original.object->scripts[0]->StartResumableFunction(
      "campaign:1", "", "Run", { VarValue(original.object.get()) },
      original.Stack());
    REQUIRE(result.continuation);
    saved = original.Save(*result.continuation);
  }
  CheckpointHost restored(Json::parse(saved));
  configure(restored);
  const auto frame = restored.heap.TakeContinuation("campaign:1", "first");
  REQUIRE(frame.locals[3].second.GetType() == VarValue::kType_Object);
  REQUIRE_FALSE(static_cast<IGameObject*>(frame.locals[3].second));
  REQUIRE(frame.locals[3].second.objectType == "Quest");
  REQUIRE(static_cast<IGameObject*>(frame.locals[4].second) ==
          restored.object.get());
  REQUIRE(frame.locals[4].second.objectType == "Quest");
  REQUIRE(restored.Resume(frame, 12).continuation);
}

TEST_CASE("Checkpoint admission does not silently enable promises or lose "
          "nested Papyrus callers",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  CheckpointHost host;
  SECTION("ordinary strict entry")
  {
    std::vector<VarValue> args;
    REQUIRE_THROWS_WITH(
      host.vm->CallMethod(host.object.get(), "Run", args),
      Catch::Matchers::ContainsSubstring("explicit host entry"));
  }
  SECTION("duplicate attachment cannot be given a stable frame identity")
  {
    host.vm->AddObject(host.object,
                       { { host.script->source, host.variables } });
    REQUIRE_THROWS_WITH(host.Start(),
                        Catch::Matchers::ContainsSubstring(
                          "Ambiguous Papyrus checkpoint attachment"));
    REQUIRE(host.Reward() == 0);
  }
  SECTION("ordinary promise in checkpoint entry")
  {
    host.vm->RegisterFunction("CheckpointHost", "Await",
                              FunctionType::GlobalFunction,
                              [](VarValue, std::vector<VarValue>) -> VarValue {
                                return VarValue(Viet::Promise<VarValue>());
                              });
    REQUIRE_THROWS_WITH(
      host.Start(),
      Catch::Matchers::ContainsSubstring("durable continuation adapter"));
  }
  SECTION("nested native wrapper")
  {
    host.vm->RegisterFunction(
      "CheckpointHost", "InnerAwait", FunctionType::GlobalFunction,
      [&](VarValue, std::vector<VarValue>) -> VarValue {
        host.vm->RequestSuspension("inner", "Int");
      });
    host.vm->RegisterFunction(
      "CheckpointHost", "Await", FunctionType::GlobalFunction,
      [&](VarValue, std::vector<VarValue> args) -> VarValue {
        return host.vm->CallStatic("CheckpointHost", "InnerAwait", args);
      });
    REQUIRE_THROWS_WITH(host.Start(),
                        Catch::Matchers::ContainsSubstring("caller chain"));
  }
  SECTION("nested script body")
  {
    auto& functions = host.script->objectTable[0].states[0].functions;
    functions.push_back({ "Inner", functions[0].function });
    functions[0].function.code.instructions = {
      { FunctionCode::kOp_CallMethod,
        { VarValue("Inner"), Name("self"), Name("result"), VarValue(0) } },
      { FunctionCode::kOp_Return, { Name("result") } }
    };
    REQUIRE_THROWS_WITH(host.Start(),
                        Catch::Matchers::ContainsSubstring("caller chain"));
  }
  SECTION("nested property handler")
  {
    auto& object = host.script->objectTable[0];
    Object::PropInfo property;
    property.name = "WaitForValue";
    property.type = "Int";
    property.flags = Object::PropInfo::kFlags_Read;
    property.readHandler = object.states[0].functions[0].function;
    object.properties.push_back(property);
    object.states[0].functions[0].function.code.instructions = {
      { FunctionCode::kOp_PropGet,
        { VarValue("WaitForValue"), Name("self"), Name("result") } },
      { FunctionCode::kOp_Return, { Name("result") } }
    };
    REQUIRE_THROWS_WITH(host.Start(),
                        Catch::Matchers::ContainsSubstring("caller chain"));
  }
  REQUIRE(host.heap.Save().at("continuations").empty());
}

TEST_CASE("Suspension refuses frames that cannot be restored",
          "[Quest][QuestVM][PapyrusContinuation]")
{
  CheckpointHost host;
  auto& code = host.script->objectTable[0]
                 .states[0]
                 .functions[0]
                 .function.code.instructions;
  SECTION("literal result operand")
  {
    code[4].args[2] = VarValue(3);
  }
  SECTION("self result operand")
  {
    code[4].args[2] = Name("self");
  }
  SECTION("state result operand")
  {
    code[4].args[2] = Name("::State");
  }
  SECTION("unsupported result declaration")
  {
    host.vm->RegisterFunction(
      "CheckpointHost", "Await", FunctionType::GlobalFunction,
      [&](VarValue, std::vector<VarValue>) -> VarValue {
        host.vm->RequestSuspension("invalid", "identifier");
      });
  }
  SECTION("incompatible destination declaration")
  {
    host.vm->RegisterFunction(
      "CheckpointHost", "Await", FunctionType::GlobalFunction,
      [&](VarValue, std::vector<VarValue>) -> VarValue {
        host.vm->RequestSuspension("invalid", "String");
      });
  }
  SECTION("local no longer matches its declaration")
  {
    code.insert(
      code.begin() + 4,
      { FunctionCode::kOp_Assign, { Name("result"), VarValue("wrong") } });
  }
  SECTION("discard local cannot hide nested array elements")
  {
    host.script->objectTable[0]
      .states[0]
      .functions[0]
      .function.locals.push_back({ "discard", "None" });
    VarValue nested(static_cast<uint8_t>(VarValue::kType_IntArray));
    nested.pArray = std::make_shared<std::vector<VarValue>>(1, VarValue(4));
    VarValue malformed(static_cast<uint8_t>(VarValue::kType_ObjectArray));
    malformed.pArray = std::make_shared<std::vector<VarValue>>(1, nested);
    *host.variables->GetVariableByName("shared", *host.script) = malformed;
    const auto await = code[4];
    code = { { FunctionCode::kOp_Assign, { Name("discard"), Name("shared") } },
             await,
             { FunctionCode::kOp_Return, { Name("result") } } };
  }
  SECTION("script array writes cannot create nested arrays")
  {
    auto& locals =
      host.script->objectTable[0].states[0].functions[0].function.locals;
    locals.push_back({ "outer", "Quest[]" });
    const auto await = code[4];
    code = { { FunctionCode::kOp_ArrayCreate, { Name("outer"), VarValue(1) } },
             { FunctionCode::kOp_ArrayCreate,
               { Name("localOnly"), VarValue(1) } },
             { FunctionCode::kOp_ArraySetElement,
               { Name("outer"), VarValue(0), Name("localOnly") } },
             await,
             { FunctionCode::kOp_Return, { Name("result") } } };
  }
  REQUIRE_THROWS(host.Start());
  REQUIRE(host.heap.Save().at("continuations").empty());
}
