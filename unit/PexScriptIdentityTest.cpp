#include "papyrus-vm/IGameObject.h"
#include "papyrus-vm/PexScript.h"
#include <catch2/catch_all.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace {
std::shared_ptr<PexScript> IdentityFixture()
{
  auto script = std::make_shared<PexScript>();
  script->source = "IdentityFixture";
  script->userFlagTable.push_back({ "conditional", 1 });
  Object object;
  object.NameIndex = script->source;
  object.parentClassName = "Quest";
  object.variables.push_back({ "counter", "int", 0, VarValue(7) });
  Object::PropInfo property;
  property.name = "Count";
  property.type = "int";
  property.flags = Object::PropInfo::kFlags_Read;
  property.readHandler.valid = true;
  property.readHandler.returnType = "int";
  property.readHandler.code.instructions = { { FunctionCode::kOp_Return,
                                               { VarValue(7) } } };
  object.properties.push_back(property);
  FunctionInfo function;
  function.valid = true;
  function.returnType = "string";
  function.params.push_back({ "input", "int" });
  function.locals.push_back({ "result", "string" });
  function.code.instructions = {
    { FunctionCode::kOp_Assign,
      { VarValue(VarValue::kType_Identifier, "result"),
        VarValue(std::string("independently allocated literal")) } },
    { FunctionCode::kOp_Return,
      { VarValue(VarValue::kType_Identifier, "result") } }
  };
  object.states.push_back({ "", { { "Run", function } } });
  script->objectTable.push_back(object);
  return script;
}

FunctionInfo& Body(PexScript& script)
{
  return script.objectTable[0].states[0].functions[0].function;
}

class IdentityRuntimeObject : public IGameObject
{
protected:
  const std::vector<std::shared_ptr<ActivePexInstance>>&
  ListActivePexInstances() const override
  {
    return instances;
  }
  void AddScript(std::shared_ptr<ActivePexInstance> instance) noexcept override
  {
    instances.push_back(std::move(instance));
  }

private:
  std::vector<std::shared_ptr<ActivePexInstance>> instances;
};
}

TEST_CASE("PEX execution identity uses canonical SHA-256 encoding",
          "[PexScript][ExecutionHash]")
{
  PexScript script;
  script.header.Signature = 0x01020304;
  script.header.VerMajor = 3;
  script.header.VerMinor = 2;
  script.header.GameID = 0x0506;
  script.source = "Fixture";
  script.userFlagTable.push_back({ "conditional", 7 });
  // Independently generated from the documented big-endian scalar/length
  // encoding, including the skymp.pex.execution.v1 domain and empty objects.
  REQUIRE(script.ExecutionHash() ==
          "ff49d42890f6442b39e95ab1d13693ab01723a4c2e30a38eebd51b9264402914");
}

TEST_CASE("PEX execution identity ignores allocation and non-executable data",
          "[PexScript][ExecutionHash]")
{
  auto first = IdentityFixture();
  auto second = IdentityFixture();
  REQUIRE(
    static_cast<const char*>(Body(*first).code.instructions[0].args[1]) !=
    static_cast<const char*>(Body(*second).code.instructions[0].args[1]));
  const auto expected = first->ExecutionHash();
  REQUIRE(expected.size() == 64);
  REQUIRE(expected.find_first_not_of("0123456789abcdef") == std::string::npos);
  REQUIRE(second->ExecutionHash() == expected);
  second->header.BuildTime = 123456789;
  second->user = "different compiler user";
  second->machine = "different compiler machine";
  second->debugInfo.m_flags = 1;
  second->debugInfo.m_sourceModificationTime = 123;
  second->debugInfo.m_data.push_back({ "o", "s", "f", 0, { 20, 30 } });
  second->stringTable.SetStorage({ "unused", "different layout" });
  second->stringTable.instanceStringTable.push_back(
    std::make_shared<std::string>("runtime allocation"));
  second->objectTable[0].docstring = "documentation";
  second->objectTable[0].properties[0].docstring = "property docs";
  second->objectTable[0].properties[0].readHandler.docstring = "getter docs";
  Body(*second).docstring = "function docs";
  Body(*second).code.instructions.reserve(100);
  REQUIRE(second->ExecutionHash() == expected);
}

TEST_CASE(
  "PEX execution identity observes current mutable executable metadata",
  "[PexScript][ExecutionHash]")
{
  using Edit = std::function<void(PexScript&)>;
  const std::vector<std::pair<const char*, Edit>> edits{
    { "source", [](auto& s) { s.source += "Other"; } },
    { "format", [](auto& s) { ++s.header.VerMinor; } },
    { "user flag name", [](auto& s) { s.userFlagTable[0].name += "Other"; } },
    { "user flag index", [](auto& s) { ++s.userFlagTable[0].idx; } },
    { "object", [](auto& s) { s.objectTable[0].NameIndex += "Other"; } },
    { "parent", [](auto& s) { s.objectTable[0].parentClassName = "Form"; } },
    { "autostate", [](auto& s) { s.objectTable[0].autoStateName = "Ready"; } },
    { "object flags", [](auto& s) { ++s.objectTable[0].userFlags; } },
    { "variable name",
      [](auto& s) { s.objectTable[0].variables[0].name += "X"; } },
    { "variable type",
      [](auto& s) { s.objectTable[0].variables[0].typeName = "float"; } },
    { "variable flags",
      [](auto& s) { ++s.objectTable[0].variables[0].userFlags; } },
    { "default",
      [](auto& s) { s.objectTable[0].variables[0].value = VarValue(8); } },
    { "property name",
      [](auto& s) { s.objectTable[0].properties[0].name += "X"; } },
    { "property type",
      [](auto& s) { s.objectTable[0].properties[0].type = "float"; } },
    { "property flags",
      [](auto& s) { ++s.objectTable[0].properties[0].flags; } },
    { "property user flags",
      [](auto& s) { ++s.objectTable[0].properties[0].userFlags; } },
    { "property binding",
      [](auto& s) {
        s.objectTable[0].properties[0].autoVarName = "counter";
      } },
    { "getter",
      [](auto& s) {
        s.objectTable[0]
          .properties[0]
          .readHandler.code.instructions[0]
          .args[0] = VarValue(8);
      } },
    { "setter",
      [](auto& s) { s.objectTable[0].properties[0].writeHandler.flags = 2; } },
    { "state", [](auto& s) { s.objectTable[0].states[0].name = "Ready"; } },
    { "function name",
      [](auto& s) {
        s.objectTable[0].states[0].functions[0].name = "Other";
      } },
    { "return type", [](auto& s) { Body(s).returnType = "int"; } },
    { "function flags", [](auto& s) { Body(s).flags = 2; } },
    { "function user flags", [](auto& s) { ++Body(s).userFlags; } },
    { "function validity", [](auto& s) { Body(s).valid = false; } },
    { "parameter name", [](auto& s) { Body(s).params[0].name += "X"; } },
    { "parameter type", [](auto& s) { Body(s).params[0].type = "float"; } },
    { "local name", [](auto& s) { Body(s).locals[0].name += "X"; } },
    { "local type", [](auto& s) { Body(s).locals[0].type = "int"; } },
    { "opcode",
      [](auto& s) {
        Body(s).code.instructions[0].op = FunctionCode::kOp_Cast;
      } },
    { "operand",
      [](auto& s) {
        Body(s).code.instructions[0].args[1] = VarValue("changed");
      } },
    { "operand type",
      [](auto& s) {
        Body(s).code.instructions[0].args[0] = VarValue("result");
      } },
    { "receiver type",
      [](auto& s) {
        Body(s).code.instructions[0].args[0].objectType = "Quest";
      } },
    { "function order",
      [](auto& s) {
        s.objectTable[0].states[0].functions.push_back({ "Second", {} });
      } },
    { "length prefixes",
      [](auto& s) {
        s.objectTable[0].NameIndex += "Q";
        s.objectTable[0].parentClassName.erase(0, 1);
      } }
  };
  for (const auto& [name, edit] : edits) {
    INFO(name);
    auto script = IdentityFixture();
    const auto before = script->ExecutionHash();
    edit(*script);
    REQUIRE(script->ExecutionHash() != before);
  }
}

TEST_CASE("PEX operand identity distinguishes scalar tags and floating bits",
          "[PexScript][ExecutionHash]")
{
  auto script = IdentityFixture();
  auto& instructions = Body(*script).code.instructions;
  instructions = { { FunctionCode::kOp_Return, { VarValue::None() } } };
  const std::vector<VarValue> values{ VarValue::None(),
                                      VarValue(0),
                                      VarValue(false),
                                      VarValue(0.0),
                                      VarValue(-0.0),
                                      VarValue("0"),
                                      VarValue(VarValue::kType_Identifier,
                                               "0"),
                                      VarValue(1.0),
                                      VarValue(std::nextafter(1.0, 2.0)) };
  std::set<std::string> hashes;
  for (const auto& value : values) {
    instructions[0].args[0] = value;
    hashes.insert(script->ExecutionHash());
  }
  REQUIRE(hashes.size() == values.size());
}

TEST_CASE("PEX execution identity rejects runtime values and malformed code",
          "[PexScript][ExecutionHash]")
{
  auto script = IdentityFixture();
  auto& function = Body(*script);
  IdentityRuntimeObject object;
  VarValue scalarArray(7);
  scalarArray.pArray = std::make_shared<std::vector<VarValue>>();
  const std::vector<VarValue> invalid{
    VarValue(&object),
    VarValue(uint8_t(VarValue::kType_IntArray)),
    scalarArray,
    VarValue(Viet::Promise<VarValue>()),
    VarValue(static_cast<const char*>(nullptr)),
    VarValue(VarValue::kType_Identifier, nullptr),
    VarValue(std::numeric_limits<double>::infinity()),
    VarValue(std::numeric_limits<double>::quiet_NaN())
  };
  for (size_t index = 0; index < invalid.size(); ++index) {
    INFO(index);
    const auto& value = invalid[index];
    function.code.instructions = { { FunctionCode::kOp_Return, { value } } };
    REQUIRE_THROWS(script->ExecutionHash());
    function.code.instructions.clear();
    auto& variables = script->objectTable[0].variables;
    variables.clear();
    variables.push_back({ "counter", "int", 0, value });
    REQUIRE_THROWS(script->ExecutionHash());
    variables.clear();
    variables.push_back({ "counter", "int", 0, VarValue(7) });
  }
  function.code.instructions = { { 255, {} } };
  REQUIRE_THROWS(script->ExecutionHash());
  function.code.instructions = { { FunctionCode::kOp_Return, {} } };
  REQUIRE_THROWS(script->ExecutionHash());
  function.code.instructions = { { FunctionCode::kOp_CallStatic,
                                   { VarValue("Utility"), VarValue("Wait"),
                                     VarValue::None(), VarValue(1) } } };
  REQUIRE_THROWS(script->ExecutionHash());
  function.code.instructions[0].args[3] = VarValue(-1);
  REQUIRE_THROWS(script->ExecutionHash());
  function.code.instructions[0].args[3] = VarValue(129);
  REQUIRE_THROWS(script->ExecutionHash());
  function.code.instructions[0].args[3] = VarValue(false);
  REQUIRE_THROWS(script->ExecutionHash());
}

TEST_CASE("PEX execution identity bounds traversal and total encoded data",
          "[PexScript][ExecutionHash]")
{
  auto script = IdentityFixture();
  SECTION("metadata string")
  {
    script->source.assign(65536, 'x');
    REQUIRE_THROWS(script->ExecutionHash());
  }
  SECTION("operand string")
  {
    Body(*script).code.instructions[0].args[1] =
      VarValue(std::string(65536, 'x'));
    REQUIRE_THROWS(script->ExecutionHash());
  }
  SECTION("collection")
  {
    Body(*script).code.instructions.resize(65536);
    REQUIRE_THROWS(script->ExecutionHash());
  }
  SECTION("total canonical stream")
  {
    // Reuse a source operand allocation: the stream still counts each use.
    const auto value = VarValue(std::string(65535, 'x'));
    Body(*script).code.instructions.assign(
      1025, { FunctionCode::kOp_Return, { value } });
    REQUIRE_THROWS(script->ExecutionHash());
  }
}
