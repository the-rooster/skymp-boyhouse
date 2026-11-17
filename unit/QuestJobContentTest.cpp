#include "QuestJobContent.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
std::string Bytes(uint32_t value, size_t count)
{
  std::string result(count, '\0');
  for (size_t i = 0; i < count; ++i)
    result[i] = static_cast<char>((value >> (i * 8)) & 255);
  return result;
}
std::string Field(const char* name, const std::string& value)
{
  return std::string(name, 4) + Bytes(uint32_t(value.size()), 2) + value;
}
std::string Record(const char* name, uint32_t id, const std::string& fields)
{
  return std::string(name, 4) + Bytes(uint32_t(fields.size()), 4) +
    Bytes(0, 4) + Bytes(id, 4) + std::string(8, '\0') + fields;
}
std::string QuestFields(uint8_t complete = 0, uint32_t condition = 1)
{
  return Field("DNAM", std::string(12, '\0')) + Field("NEXT", "") +
    Field("INDX", Bytes(10, 2) + Bytes(0, 2)) +
    Field("QSDT", Bytes(complete, 1)) +
    Field("CTDA", Bytes(condition, 4) + std::string(28, '\0')) +
    Field("ANAM", Bytes(0, 4));
}
std::string Plugin(const std::string& quest = QuestFields(), int global = 3)
{
  return Record("TES4", 0, "") + Record("QUST", 0x100, quest) +
    Record("GLOB", 0x200, Field("FLTV", Bytes(global, 4)));
}
struct ContentFiles
{
  std::filesystem::path directory;
  std::string filename;
  explicit ContentFiles(const std::string& bytes,
                        const std::string& name = "ContentFixture.esm")
    : filename(name)
  {
    std::random_device random;
    for (size_t attempt = 0; attempt < 20 && directory.empty(); ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
        ("skymp-content-unit-" + std::to_string(random()) + "-" +
         std::to_string(random()));
      if (std::filesystem::create_directory(candidate))
        directory = candidate;
    }
    if (directory.empty())
      throw std::runtime_error("Cannot create content fixture directory");
    Write(bytes);
  }
  std::filesystem::path File() const { return directory / filename; }
  void Write(const std::string& bytes) const
  {
    std::ofstream out(File(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out)
      throw std::runtime_error("Cannot write content fixture");
  }
  ~ContentFiles()
  {
    std::error_code error;
    std::filesystem::remove(File(), error);
    std::filesystem::remove(directory, error);
  }
};
std::shared_ptr<PexScript> Script(const std::string& name)
{
  auto script = std::make_shared<PexScript>();
  script->source = name;
  Object object;
  object.NameIndex = name;
  object.variables.push_back({ "count", "Int", 0, VarValue(7) });
  FunctionInfo function;
  function.valid = true;
  function.returnType = "Int";
  function.code.instructions = { { FunctionCode::kOp_Return, { VarValue(7) } } };
  object.states.push_back({ "", { { "Fragment", function } } });
  script->objectTable.push_back(object);
  return script;
}
PexScript::Lazy Lazy(const std::shared_ptr<PexScript>& script)
{
  return { script->source, [script] { return script; } };
}
espm::QuestScriptData Metadata()
{
  espm::QuestScriptData metadata;
  metadata.version = 5;
  metadata.objectFormat = 2;
  metadata.hasFragments = true;
  metadata.bindVersion = 2;
  metadata.fileName = "QF_ContentFixture";
  espm::QuestScriptValue number;
  number.type = 3;
  number.integer = 7;
  espm::QuestScriptValue array;
  array.type = 13;
  array.elements = { number, number };
  metadata.scripts.push_back(
    { "ContentFixture", 0, { { "Count", 0, number }, { "Array", 1, array } } });
  metadata.fragments.push_back({ 10, 0, 1, "ContentFixture", "Fragment" });
  espm::QuestScriptData::Alias alias;
  alias.object.type = 1;
  alias.object.formId = 0x100;
  alias.object.alias = 1;
  alias.version = 5;
  alias.objectFormat = 2;
  alias.scripts.push_back({ "AliasFixture", 0, {} });
  metadata.aliases.push_back(alias);
  return metadata;
}
struct ContentFixture
{
  ContentFiles files;
  espm::Loader loader;
  PartOne server;
  std::shared_ptr<PexScript> script = Script("ContentFixture");
  std::shared_ptr<PexScript> alias = Script("AliasFixture");
  espm::QuestScriptData metadata = Metadata();
  explicit ContentFixture(const std::string& bytes = Plugin())
    : files(bytes)
    , loader(std::vector<std::filesystem::path>{ files.File() }, nullptr,
              espm::Loader::BufferType::AllocatedBuffer)
  {
    server.AttachEspm(&loader);
  }
  std::vector<PexScript::Lazy> Scripts() { return { Lazy(script), Lazy(alias) }; }
  std::string Hash()
  {
    return QuestJobContent::Hash(server.worldState, 0x100, metadata, Scripts());
  }
};
}

TEST_CASE("Complete master SHA256 cache is immutable and thread safe",
          "[Quest][QuestJobContent][LoaderContentHash]")
{
  ContentFiles file(Record("TES4", 0, ""));
  espm::Loader loader(std::vector<std::filesystem::path>{ file.File() },
                       nullptr, espm::Loader::BufferType::AllocatedBuffer);
  std::vector<std::future<const std::vector<espm::Loader::FileContentHash>*>>
    calls;
  for (size_t i = 0; i < 8; ++i)
    calls.push_back(std::async(std::launch::async,
                              [&] { return &loader.GetContentHashes(); }));
  const auto* hashes = calls[0].get();
  for (size_t i = 1; i < calls.size(); ++i)
    REQUIRE(calls[i].get() == hashes);
  REQUIRE(hashes->size() == 1);
  REQUIRE(hashes->front().fileName == "ContentFixture.esm");
  REQUIRE(hashes->front().size == 24);
  // Independent SHA-256 vector: ASCII TES4 followed by twenty zero bytes.
  REQUIRE(hashes->front().sha256 ==
          "dbcceafb84d2a9be6065e1253a067d188b7e2bd1346972b4e30694de6519fd36");
  file.Write(Plugin());
  REQUIRE(&loader.GetContentHashes() == hashes);
  REQUIRE(hashes->front().size == 24); // Never reopen the changed path.
  espm::Loader replacement(std::vector<std::filesystem::path>{ file.File() },
                            nullptr, espm::Loader::BufferType::AllocatedBuffer);
  REQUIRE(replacement.GetContentHashes().front().sha256 !=
          hashes->front().sha256);
}

TEST_CASE("Complete master SHA256 cache preserves load order",
          "[Quest][QuestJobContent][LoaderContentHash]")
{
  ContentFiles first(Record("TES4", 0, ""), "First.esm");
  ContentFiles second(Plugin(), "Second.esm");
  espm::Loader forward(
    std::vector<std::filesystem::path>{ first.File(), second.File() }, nullptr,
    espm::Loader::BufferType::AllocatedBuffer);
  espm::Loader reverse(
    std::vector<std::filesystem::path>{ second.File(), first.File() }, nullptr,
    espm::Loader::BufferType::AllocatedBuffer);
  const auto& a = forward.GetContentHashes();
  const auto& b = reverse.GetContentHashes();
  REQUIRE(a.size() == 2);
  REQUIRE(b.size() == 2);
  REQUIRE(a[0].fileName == "First.esm");
  REQUIRE(a[1].fileName == "Second.esm");
  REQUIRE(b[0].fileName == "Second.esm");
  REQUIRE(b[1].fileName == "First.esm");
  REQUIRE(a[0].sha256 == b[1].sha256);
  REQUIRE(a[1].sha256 == b[0].sha256);
  REQUIRE(a[0].sha256 != a[1].sha256);
}

TEST_CASE("Quest script manifests ignore input order but retain executable data",
          "[Quest][QuestJobContent]")
{
  ContentFixture fixture;
  auto scripts = fixture.Scripts();
  const auto manifest = QuestJobContent::ScriptManifest(scripts);
  REQUIRE(manifest.size() == 64);
  REQUIRE(manifest.find_first_not_of("0123456789abcdef") == std::string::npos);
  std::reverse(scripts.begin(), scripts.end());
  scripts[0].source = "ALIASFIXTURE";
  REQUIRE(QuestJobContent::ScriptManifest(scripts) == manifest);
  fixture.script->header.BuildTime = 12345;
  fixture.script->debugInfo.m_sourceModificationTime = 67890;
  fixture.script->user = "other builder";
  fixture.script->objectTable[0].docstring = "different docs";
  REQUIRE(QuestJobContent::ScriptManifest(scripts) == manifest);
  const auto baseline = fixture.Hash();
  REQUIRE(QuestJobContent::Hash(fixture.server.worldState, 0x100,
                                fixture.metadata, manifest) == baseline);
  fixture.script->objectTable[0].states[0].functions[0]
    .function.code.instructions[0].args[0] = VarValue(8);
  REQUIRE(QuestJobContent::ScriptManifest(scripts) != manifest);
  REQUIRE(fixture.Hash() != baseline);
}

TEST_CASE("Quest content pins supplied VMAD properties fragments and aliases",
          "[Quest][QuestJobContent]")
{
  ContentFixture fixture;
  const auto baseline = fixture.Hash();
  const auto original = fixture.metadata;
  using Edit = std::function<void(espm::QuestScriptData&)>;
  const std::vector<Edit> edits{
    [](auto& m) { ++m.version; },
    [](auto& m) { ++m.objectFormat; },
    [](auto& m) { m.hasFragments = false; },
    [](auto& m) { ++m.bindVersion; },
    [](auto& m) { m.fileName += "X"; },
    [](auto& m) { m.scripts[0].name += "X"; },
    [](auto& m) { ++m.scripts[0].status; },
    [](auto& m) { m.scripts[0].properties[0].name += "X"; },
    [](auto& m) { ++m.scripts[0].properties[0].status; },
    [](auto& m) { ++m.scripts[0].properties[0].value.integer; },
    [](auto& m) { ++m.scripts[0].properties[1].value.elements[1].integer; },
    [](auto& m) { ++m.fragments[0].stage; },
    [](auto& m) { ++m.fragments[0].logEntry; },
    [](auto& m) { ++m.fragments[0].flags; },
    [](auto& m) { m.fragments[0].script += "X"; },
    [](auto& m) { m.fragments[0].function += "X"; },
    [](auto& m) { ++m.aliases[0].object.alias; },
    [](auto& m) { ++m.aliases[0].object.formId; },
    [](auto& m) { ++m.aliases[0].version; },
    [](auto& m) { ++m.aliases[0].objectFormat; },
    [](auto& m) { ++m.aliases[0].scripts[0].status; }
  };
  for (size_t i = 0; i < edits.size(); ++i) {
    INFO("metadata edit " << i);
    fixture.metadata = original;
    edits[i](fixture.metadata);
    REQUIRE(fixture.Hash() != baseline);
  }
}

TEST_CASE("Quest content pins raw logs plugin mapping and other master records",
          "[Quest][QuestJobContent]")
{
  ContentFixture original;
  const auto baseline = original.Hash();
  SECTION("QSDT completion")
  {
    ContentFixture changed(Plugin(QuestFields(1)));
    REQUIRE(changed.Hash() != baseline);
  }
  SECTION("log condition")
  {
    ContentFixture changed(Plugin(QuestFields(0, 2)));
    REQUIRE(changed.Hash() != baseline);
  }
  SECTION("unknown raw quest field")
  {
    ContentFixture changed(Plugin(QuestFields() + Field("FUTR", "value")));
    REQUIRE(changed.Hash() != baseline);
  }
  SECTION("unrelated global record")
  {
    ContentFixture changed(Plugin(QuestFields(), 4));
    REQUIRE(changed.Hash() != baseline);
  }
  SECTION("FormDesc plugin order")
  {
    original.server.worldState.espmFiles.push_back("Other.esm");
    const auto firstOrder = original.Hash();
    std::reverse(original.server.worldState.espmFiles.begin(),
                  original.server.worldState.espmFiles.end());
    REQUIRE(original.Hash() != firstOrder);
  }
}

TEST_CASE("Content pinning rejects incomplete ambiguous or unbounded inputs",
          "[Quest][QuestJobContent]")
{
  ContentFixture fixture;
  auto scripts = fixture.Scripts();
  SECTION("case insensitive duplicate")
  {
    auto duplicate = scripts.front();
    duplicate.source = "CONTENTFIXTURE";
    scripts.push_back(duplicate);
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("missing loader")
  {
    scripts[0].fn = {};
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("null script")
  {
    scripts[0].fn = [] { return std::shared_ptr<PexScript>{}; };
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("mismatched source")
  {
    scripts[0].source = "Different";
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("empty script name")
  {
    scripts[0].source.clear();
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("oversized script list")
  {
    scripts.resize(65536);
    REQUIRE_THROWS(QuestJobContent::ScriptManifest(scripts));
  }
  SECTION("oversized metadata string")
  {
    fixture.metadata.fileName.assign(65536, 'x');
    REQUIRE_THROWS(fixture.Hash());
  }
  SECTION("recursive metadata array")
  {
    fixture.metadata.scripts[0].properties[1].value.elements[0].type = 13;
    REQUIRE_THROWS(fixture.Hash());
  }
  SECTION("nonfinite property")
  {
    fixture.metadata.scripts[0].properties[0].value.real =
      std::numeric_limits<float>::infinity();
    REQUIRE_THROWS(fixture.Hash());
  }
  SECTION("invalid digest")
  {
    REQUIRE_THROWS(QuestJobContent::Hash(fixture.server.worldState, 0x100,
                                         fixture.metadata, std::string(64, 'g')));
  }
  SECTION("nonquest record")
  {
    REQUIRE_THROWS(QuestJobContent::Hash(fixture.server.worldState, 0x200,
                                         fixture.metadata, scripts));
  }
}

TEST_CASE(
  "Quest override manifests pin every child independently of visit order",
  "[Quest][QuestJobContent]")
{
  ContentFixture fixture(Plugin() + Record("QUST", 0x300, QuestFields()));
  const auto manifest = QuestJobContent::ScriptManifest(fixture.Scripts());
  REQUIRE(QuestJobContent::WithMetadataOverrides(fixture.server.worldState,
                                                 manifest, {}) == manifest);
  using Overrides = std::map<uint32_t, espm::QuestScriptData>;
  Overrides overrides{ { 0x100, fixture.metadata },
                       { 0x300, fixture.metadata } };
  const auto pinned = QuestJobContent::WithMetadataOverrides(
    fixture.server.worldState, manifest, overrides);
  REQUIRE(pinned != manifest);
  Overrides reversed;
  reversed.emplace(0x300, fixture.metadata);
  reversed.emplace(0x100, fixture.metadata);
  REQUIRE(QuestJobContent::WithMetadataOverrides(
            fixture.server.worldState, manifest, reversed) == pinned);
  const auto rootHash = QuestJobContent::Hash(fixture.server.worldState, 0x100,
                                              fixture.metadata, pinned);
  // A retired child's fixture-only VMAD edit still changes the root's pin.
  ++overrides.at(0x300).scripts[0].properties[0].value.integer;
  const auto changed = QuestJobContent::WithMetadataOverrides(
    fixture.server.worldState, manifest, overrides);
  REQUIRE(changed != pinned);
  REQUIRE(QuestJobContent::Hash(fixture.server.worldState, 0x100,
                                fixture.metadata, changed) != rootHash);
  overrides.erase(0x300);
  REQUIRE(QuestJobContent::WithMetadataOverrides(
            fixture.server.worldState, manifest, overrides) != pinned);
}

TEST_CASE("Quest override manifests reject invalid maps and source digests",
          "[Quest][QuestJobContent]")
{
  ContentFixture fixture;
  const auto manifest = QuestJobContent::ScriptManifest(fixture.Scripts());
  std::map<uint32_t, espm::QuestScriptData> overrides;
  SECTION("invalid empty-map source")
  {
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, std::string(64, 'g'), overrides));
  }
  SECTION("invalid populated-map source")
  {
    overrides.emplace(0x100, fixture.metadata);
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, "short", overrides));
  }
  SECTION("nonquest record")
  {
    overrides.emplace(0x200, fixture.metadata);
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, manifest, overrides));
  }
  SECTION("zero identity")
  {
    overrides.emplace(0, fixture.metadata);
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, manifest, overrides));
  }
  SECTION("missing quest")
  {
    overrides.emplace(0x300, fixture.metadata);
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, manifest, overrides));
  }
  SECTION("oversized map")
  {
    for (uint32_t id = 0; id < 4097; ++id)
      overrides.emplace(id, espm::QuestScriptData{});
    REQUIRE_THROWS(QuestJobContent::WithMetadataOverrides(
      fixture.server.worldState, manifest, overrides));
  }
}
