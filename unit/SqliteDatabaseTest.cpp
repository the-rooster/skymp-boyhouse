#include "database_drivers/SqliteDatabase.h"
#include "CampaignReferenceBinding.h"
#include "QuestJournal.h"
#include "SaveReceipt.h"
#include "TestUtils.hpp"
#include "database_drivers/DatabaseFactory.h"
#include "save_storages/AsyncSaveStorage.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sqlite3.h>
#include <thread>
#if defined(__linux__)
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

extern espm::Loader& GetEspmLoader();

namespace {
using Storage =
  Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>;

// Delete only the exclusively created test directory, never an existing path.
struct SqliteFiles
{
  std::filesystem::path directory;
  SqliteFiles()
  {
    std::random_device random;
    for (int attempt = 0; attempt < 20; ++attempt) {
      auto candidate = std::filesystem::temp_directory_path() /
        ("skymp-sqlite-unit-" + std::to_string(random()) + "-" +
         std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error(
      "Unable to create an isolated SQLite test directory");
  }
  ~SqliteFiles()
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  std::string File() const { return (directory / "world.sqlite3").string(); }
};

struct RawDatabase
{
  sqlite3* db = nullptr;
  explicit RawDatabase(const std::string& file)
  {
    if (sqlite3_open(file.c_str(), &db) != SQLITE_OK) {
      if (db)
        sqlite3_close(db);
      throw std::runtime_error("Cannot open SQLite test fixture");
    }
  }
  ~RawDatabase() { sqlite3_close(db); }
  void Execute(const char* sql)
  {
    const auto result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
};

MpChangeForm SavedForm(uint32_t id, uint32_t count = 1)
{
  MpChangeForm result;
  result.formDesc = { id, "" };
  result.baseDesc = { 0x7, "Skyrim.esm" };
  result.worldOrCellDesc = FormDesc::Tamriel();
  result.worldInstance = "campaign-owner:1";
  result.inv.AddItem(0xf, count);
  result.dynamicFields.SetValueDump(
    "private.boyhouse.questJournal",
    R"({"schema":4,"scripts":{"reference:a:Skyrim.esm/test":{"counter":12}},"globals":{"12:Skyrim.esm":3.5}})");
  return result;
}

std::map<std::string, nlohmann::json> Read(
  SqliteDatabase& database,
  std::optional<std::vector<FormDesc>> filter = std::nullopt)
{
  std::map<std::string, nlohmann::json> result;
  database.Iterate(
    [&](const MpChangeForm& form) {
      if (!result.emplace(form.formDesc.ToString(), MpChangeForm::ToJson(form))
             .second)
        throw std::runtime_error("Duplicate iterate result");
    },
    std::move(filter));
  return result;
}
}

TEST_CASE("SQLite saves complete change forms and reuses batches",
          "[SQLite][save]")
{
  SqliteFiles files;
  auto first = SavedForm(1, 10);
  auto second = SavedForm(2, 20);
  first.recType = MpChangeForm::ACHR;
  first.position = { 10, 20, 30 };
  first.isDead = true;
  first.actorValues.healthPercentage = 0;
  second.isDisabled = true;
  {
    SqliteDatabase database(files.File());
    REQUIRE(database.Upsert({ first, std::nullopt, second }) == 2);
    REQUIRE(Read(database).size() == 2);
    REQUIRE(Read(database).at("1") == MpChangeForm::ToJson(first));
    std::vector<std::optional<MpChangeForm>> recycled;
    REQUIRE(database.GetRecycledChangeFormsBuffer(recycled));
    REQUIRE(recycled.size() == 3);
    REQUIRE(std::all_of(recycled.begin(), recycled.end(),
                        [](const auto& form) { return !form; }));
    second.inv.AddItem(0xf, 7);
    REQUIRE(database.Upsert({ second }) == 1);
    REQUIRE(database.Upsert({ std::nullopt }) == 0);
    REQUIRE(database.Upsert({}) == 0);
  }
  SqliteDatabase reopened(files.File());
  const auto restored = Read(reopened);
  REQUIRE(restored.size() == 2);
  REQUIRE(restored.at("1") == MpChangeForm::ToJson(first));
  REQUIRE(restored.at("2") == MpChangeForm::ToJson(second));
  const auto selected =
    Read(reopened,
         std::vector<FormDesc>{ first.formDesc, first.formDesc, { 999, "" } });
  REQUIRE(selected.size() == 1);
  REQUIRE(selected.at("1") == restored.at("1"));
  REQUIRE(Read(reopened, std::vector<FormDesc>{}).empty());
}

TEST_CASE(
  "Attribute persistence accepts legacy saves and rejects incomplete values",
  "[save][ReferenceLifecycle]")
{
  auto saved = SavedForm(1);
  saved.actorValues.health = 314;
  saved.actorValues.magicka = 271;
  saved.actorValues.stamina = 161;
  saved.actorValues.healRate = 2;
  saved.actorValues.magickaRate = 4;
  saved.actorValues.staminaRate = 6;
  saved.actorValues.healRateMult = 50;
  saved.actorValues.magickaRateMult = 75;
  saved.actorValues.staminaRateMult = 125;
  saved.actorValues.healthPercentage = 0.25f;
  saved.factions = std::vector<Faction>{};
  auto json = MpChangeForm::ToJson(saved);
  const auto decode = [](const nlohmann::json& data) {
    simdjson::dom::parser parser;
    const auto text = data.dump();
    auto document = parser.parse(text).value();
    return MpChangeForm::JsonToChangeForm(document);
  };
  SECTION("full round trip")
  {
    const auto restored = decode(json);
    REQUIRE(restored.actorValues.ToTuple() == saved.actorValues.ToTuple());
    REQUIRE(restored.factions.has_value());
    REQUIRE(restored.factions->empty());
  }
  SECTION("legacy attributes keep their defaults and old percentages")
  {
    json.erase("actorValues");
    const auto restored = decode(json);
    auto expected = ActorValues();
    expected.healthPercentage = 0.25f;
    REQUIRE(restored.actorValues.ToTuple() == expected.ToTuple());
  }
  SECTION("missing member")
  {
    json["actorValues"].erase("health");
    REQUIRE_THROWS(decode(json));
  }
  SECTION("invalid member")
  {
    json["actorValues"]["health"] = "314";
    REQUIRE_THROWS(decode(json));
  }
  SECTION("null attributes")
  {
    json["actorValues"] = nullptr;
    REQUIRE_THROWS(decode(json));
  }
}

TEST_CASE("SQLite rolls back every member of a failed save batch",
          "[SQLite][save]")
{
  SqliteFiles files;
  SqliteDatabase database(files.File());
  database.Upsert({ SavedForm(1, 10), SavedForm(2, 20) });
  const auto before = Read(database);
  RawDatabase control(files.File());
  control.Execute("CREATE TRIGGER fail_second BEFORE UPDATE ON change_forms "
                  "WHEN NEW.form_desc='2' BEGIN SELECT RAISE(ABORT,'injected "
                  "disk write failure'); END");
  std::vector<std::optional<MpChangeForm>> batch{
    SavedForm(1, 100), std::nullopt, SavedForm(3, 300), SavedForm(2, 200)
  };
  const auto expected = batch;
  try {
    database.Upsert(std::move(batch));
    FAIL("The injected failure must abort the save");
  } catch (const Storage::UpsertFailedException& error) {
    REQUIRE(std::string(error.what()).find("injected disk write failure") !=
            std::string::npos);
    REQUIRE(error.GetAffectedForms() == expected);
    batch = error.GetAffectedForms();
  }
  REQUIRE(Read(database) == before);
  SqliteDatabase independent(files.File());
  REQUIRE(Read(independent) == before);
  control.Execute("DROP TRIGGER fail_second");
  REQUIRE(database.Upsert(std::move(batch)) == 3);
  REQUIRE(Read(independent).size() == 3);
  REQUIRE(Read(independent).at("1") ==
          MpChangeForm::ToJson(SavedForm(1, 100)));
  REQUIRE_THROWS_AS(database.Upsert({ SavedForm(1, 400), SavedForm(1, 500) }),
                    Storage::UpsertFailedException);
  REQUIRE(Read(independent).at("1") ==
          MpChangeForm::ToJson(SavedForm(1, 100)));
}

TEST_CASE("SQLite readers observe one snapshot across concurrent commits",
          "[SQLite][save]")
{
  SqliteFiles files;
  SqliteDatabase reader(files.File());
  SqliteDatabase writer(files.File());
  std::optional<std::vector<FormDesc>> filter;
  SECTION("whole database")
  {
  }
  SECTION("separate filtered statements")
  {
    filter = std::vector<FormDesc>{ { 1, "" }, { 2, "" } };
  }
  reader.Upsert({ SavedForm(1, 1), SavedForm(2, 1) });
  size_t count = 0;
  reader.Iterate(
    [&](const MpChangeForm& form) {
      REQUIRE(form.inv.GetItemCount(0xf) == 1);
      if (++count == 1)
        writer.Upsert({ SavedForm(1, 2), SavedForm(2, 2) });
    },
    filter);
  REQUIRE(count == 2);
  reader.Iterate(
    [&](const MpChangeForm& form) {
      REQUIRE(form.inv.GetItemCount(0xf) == 2);
    },
    filter);
}

TEST_CASE("SQLite rejects foreign databases and unsupported save versions",
          "[SQLite][save]")
{
  SqliteFiles files;
  RawDatabase control(files.File());
  SECTION("foreign database")
  {
    control.Execute(
      "CREATE TABLE foreign_data(value); INSERT INTO foreign_data VALUES(42)");
  }
  SECTION("foreign table resembles a SQLite internal name")
  {
    control.Execute(
      "CREATE TABLE sqliteXdata(value); INSERT INTO sqliteXdata VALUES(42)");
  }
  SECTION("future save format")
  {
    control.Execute("PRAGMA application_id=0x534b4d50; PRAGMA user_version=2; "
                    "CREATE TABLE change_forms(form_desc,body)");
  }
  const auto bytes = [&] {
    std::ifstream input(files.File(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto before = bytes();
  REQUIRE_THROWS_WITH(
    SqliteDatabase(files.File()),
    "Unrecognized SQLite database or unsupported schema version");
  // Refusing the file must leave its tables, rows and metadata untouched.
  int tables = 0;
  REQUIRE(sqlite3_exec(
            control.db, "SELECT name FROM sqlite_schema WHERE type='table'",
            [](void* output, int, char**, char**) {
              ++*static_cast<int*>(output);
              return 0;
            },
            &tables, nullptr) == SQLITE_OK);
  REQUIRE(tables == 1);
  REQUIRE(bytes() == before);
  REQUIRE_THROWS_AS(SqliteDatabase(":" + std::string("memory:")),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(SqliteDatabase(""), std::invalid_argument);
}

TEST_CASE("SQLite never silently skips corrupt or mismatched saved forms",
          "[SQLite][save]")
{
  SqliteFiles files;
  SqliteDatabase database(files.File());
  database.Upsert({ SavedForm(1) });
  RawDatabase control(files.File());
  SECTION("malformed body")
  {
    control.Execute("UPDATE change_forms SET body='{' WHERE form_desc='1'");
  }
  SECTION("mismatched identity")
  {
    control.Execute(
      "UPDATE change_forms SET form_desc='2' WHERE form_desc='1'");
  }
  SECTION("non-text body")
  {
    control.Execute("UPDATE change_forms SET body=x'00' WHERE form_desc='1'");
  }
  REQUIRE_THROWS_AS(Read(database), Storage::IterateFailedException);
  // A failed read rolls back its transaction, allowing repair and another
  // save.
  control.Execute("DELETE FROM change_forms");
  REQUIRE(database.Upsert({ SavedForm(3) }) == 1);
  REQUIRE(Read(database).size() == 1);
  const std::vector<FormDesc> filter{ { 3, "" } };
  try {
    database.Iterate(
      [](const MpChangeForm&) {
        throw std::runtime_error("callback failure");
      },
      filter);
    FAIL("A callback error must propagate");
  } catch (const Storage::IterateFailedException& error) {
    REQUIRE(error.GetFilter() == filter);
  }
  REQUIRE(Read(database).size() == 1);
}

TEST_CASE(
  "SQLite factory and asynchronous world saves use the transactional driver",
  "[SQLite][save]")
{
  SqliteFiles files;
  auto database = DatabaseFactory::Create(
    { { "databaseDriver", "sqlite" }, { "databaseName", files.File() } },
    spdlog::default_logger());
  REQUIRE(std::dynamic_pointer_cast<SqliteDatabase>(database));
  auto storage =
    std::make_shared<Storage>(database, spdlog::default_logger(), "sqlite", 1);
  PartOne world;
  world.AttachSaveStorage(storage);
  world.CreateActor(0xffaaaeee, { 1, 2, 3 }, 1, 0x3c);
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (storage->GetNumFinishedUpserts() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    world.worldState.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(storage->GetNumFinishedUpserts() > 0);
  SqliteDatabase reopened(files.File());
  const auto rows = Read(reopened);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows.begin()->second.at("position") ==
          nlohmann::json::array({ 1, 2, 3 }));
}

TEST_CASE("Campaign reward transactions reload journals and bound inventories "
          "together",
          "[SQLite][Quest][CampaignInventory]")
{
  SqliteFiles files;
  constexpr uint32_t ownerId = 0xff002001, npcId = 0xff002010;
  {
    auto storage =
      std::make_shared<Storage>(std::make_shared<SqliteDatabase>(files.File()),
                                spdlog::default_logger(), "sqlite", 1);
    REQUIRE(storage->SupportsAtomicBatches());
    PartOne server;
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(ownerId, { 1, 2, 3 }, 0, 0x3c);
    auto& owner = server.worldState.GetFormAt<MpActor>(ownerId);
    owner.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\"]", false, false);
    const auto instance = "campaign:" + std::to_string(ownerId) + ":fixture";
    owner.SetWorldInstance(instance);
    owner.SetInventory(Inventory().AddItem(0xf, 10));
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x13bbd),
      npcId);
    auto& npc = server.worldState.GetFormAt<MpActor>(npcId);
    npc.SetWorldInstance(instance);
    npc.SetInventory(Inventory().AddItem(0xf, 20));
    CampaignReferenceBinding::Bind(owner, 0x1a677, npc);
    QuestJournal::Atomic(owner, [&] {
      QuestJournal::Apply(
        owner,
        { { "quest", 0x4e50d }, { "op", "setStage" }, { "index", 10 } });
      QuestJournal::SetScriptState(owner, { { "fixture", 42 } });
      QuestJournal::EditInventory(owner, owner,
                                  [](Inventory& inv) { inv.AddItem(0xf, 6); });
      QuestJournal::EditInventory(
        owner, npc, [](Inventory& inv) { inv.RemoveItems({ { 0xf, 6 } }); });
    });
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (storage->GetNumFinishedUpserts() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(storage->GetNumFinishedUpserts() > 0);
  }
  auto storage =
    std::make_shared<Storage>(std::make_shared<SqliteDatabase>(files.File()),
                              spdlog::default_logger(), "sqlite", 1);
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  restarted.AttachSaveStorage(storage);
  auto& owner = restarted.worldState.GetFormAt<MpActor>(ownerId);
  auto& npc = restarted.worldState.GetFormAt<MpActor>(npcId);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 16);
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 14);
  REQUIRE(QuestJournal::Get(owner, 0x4e50d).at("stage") == 10);
  REQUIRE(QuestJournal::ScriptState(owner).at("fixture") == 42);
  REQUIRE(&CampaignReferenceBinding::Resolve(owner, 0x1a677) == &npc);
}

TEST_CASE(
  "A durable campaign receipt certifies complete SQLite rows and restart",
  "[SQLite][Quest][CampaignInventory][SaveReceipt]")
{
  SqliteFiles files;
  constexpr uint32_t ownerId = 0xff004001, npcId = 0xff004010;
  nlohmann::json journal;
  std::shared_ptr<const SaveReceipt> receipt;
  {
    auto storage =
      std::make_shared<Storage>(std::make_shared<SqliteDatabase>(files.File()),
                                spdlog::default_logger(), "sqlite-receipt", 1);
    REQUIRE(storage->SupportsAtomicBatches());
    REQUIRE(storage->SupportsUpsertResults());
    PartOne server;
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(ownerId, { 1, 2, 3 }, 0, 0x3c);
    auto& owner = server.worldState.GetFormAt<MpActor>(ownerId);
    owner.SetPropertyValueDump(QuestJournal::kEnabled,
                               "[\"4e50d:Skyrim.esm\"]", false, false);
    owner.SetWorldInstance("campaign:" + std::to_string(ownerId) + ":receipt");
    owner.SetInventory(Inventory().AddItem(0xf, 10));
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x13bbd),
      npcId);
    auto& npc = server.worldState.GetFormAt<MpActor>(npcId);
    npc.SetWorldInstance(owner.GetWorldInstance());
    npc.SetInventory(Inventory().AddItem(0xf, 20));
    CampaignReferenceBinding::Bind(owner, 0x1a677, npc);
    receipt = QuestJournal::AtomicDurable(owner, [&] {
      QuestJournal::Apply(
        owner,
        { { "quest", 0x4e50d }, { "op", "setStage" }, { "index", 10 } });
      QuestJournal::SetScriptState(owner,
                                   { { "committedToken", "reward-once" } });
      QuestJournal::EditInventory(
        owner, owner, [](Inventory& inventory) { inventory.AddItem(0xf, 6); });
      QuestJournal::EditInventory(owner, npc, [](Inventory& inventory) {
        inventory.RemoveItems({ { 0xf, 6 } });
      });
    });
    journal = QuestJournal::Snapshot(owner);
    const auto committedOwner = MpChangeForm::ToJson(owner.GetChangeForm());
    const auto committedNpc = MpChangeForm::ToJson(npc.GetChangeForm());
    REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
    REQUIRE(receipt->GetCampaign() == owner.GetChangeForm().formDesc);
    REQUIRE(receipt->GetJournalRevision() ==
            journal.at("revision").get<uint64_t>());
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (receipt->GetState() == SaveReceipt::State::Pending &&
           std::chrono::steady_clock::now() < deadline) {
      server.worldState.Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
    REQUIRE(receipt->GetAttempts() == 1);
    REQUIRE_FALSE(receipt->GetLastError());
    // A separate connection must observe the whole journal/reward commit as
    // soon as the receipt says Durable, before the writer is closed.
    SqliteDatabase independent(files.File());
    const auto rows = Read(independent);
    const auto ownerKey = owner.GetChangeForm().formDesc.ToString();
    const auto npcKey = npc.GetChangeForm().formDesc.ToString();
    REQUIRE(rows.at(ownerKey) == committedOwner);
    REQUIRE(rows.at(npcKey) == committedNpc);
  }
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  auto storage =
    std::make_shared<Storage>(std::make_shared<SqliteDatabase>(files.File()),
                              spdlog::default_logger(), "sqlite-restarted", 1);
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  restarted.AttachSaveStorage(storage);
  auto& owner = restarted.worldState.GetFormAt<MpActor>(ownerId);
  auto& npc = restarted.worldState.GetFormAt<MpActor>(npcId);
  REQUIRE(owner.GetInventory().GetItemCount(0xf) == 16);
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 14);
  auto restoredJournal = QuestJournal::Snapshot(owner);
  // Context epochs describe live instance lifetimes, not saved campaign
  // progress. A fresh server rebinds that context while restoring history.
  REQUIRE(restoredJournal.at("contextEpoch") != journal.at("contextEpoch"));
  restoredJournal.erase("contextEpoch");
  journal.erase("contextEpoch");
  REQUIRE(restoredJournal == journal);
  REQUIRE(QuestJournal::ScriptState(owner).at("committedToken") ==
          "reward-once");
  REQUIRE(&CampaignReferenceBinding::Resolve(owner, 0x1a677) == &npc);
}

TEST_CASE(
  "Private campaign NPCs and placed items restore without world side effects",
  "[SQLite][Quest][ReferenceLifecycle]")
{
  SqliteFiles files;
  constexpr uint32_t ownerId = 0xff002001, npcId = 0xff002010,
                     itemId = 0xff002011;
  MpChangeForm savedNpc, savedItem;
  {
    PartOne server;
    server.AttachEspm(&GetEspmLoader());
    server.CreateActor(ownerId, { 1, 2, 3 }, 0, 0x3c);
    auto& owner = server.worldState.GetFormAt<MpActor>(ownerId);
    const auto phase = "campaign:" + std::to_string(ownerId) + ":fixture";
    owner.SetWorldInstance(phase);
    server.worldState.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x13bbd),
      npcId);
    auto& npc = server.worldState.GetFormAt<MpActor>(npcId);
    npc.SetWorldInstance(phase);
    npc.SetInventory(Inventory().AddItem(0xf, 19));
    CampaignReferenceBinding::Bind(owner, 0x1a677, npc);
    savedNpc = npc.GetChangeForm();
    savedNpc.isDead = true;
    savedNpc.isRaceMenuOpen = false;
    savedNpc.actorValues.health = 314;
    savedNpc.actorValues.healthPercentage = 0;
    savedNpc.actorValues.magicka = 271;
    savedNpc.actorValues.magickaPercentage = 0.25f;
    savedNpc.factions = std::vector<Faction>{};
    savedNpc.equipment.numChanges = 31;
    savedNpc.equipment.rightSpell = 0x12fd0;

    const auto source = GetEspmLoader().GetBrowser().LookupById(0x8a986);
    const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                        ->GetData(server.worldState.GetEspmCache());
    const auto base = source.ToGlobalId(data.baseId);
    server.worldState.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), base, "AMMO"),
      itemId);
    auto& item = server.worldState.GetFormAt<MpObjectReference>(itemId);
    item.SetWorldInstance(phase);
    item.SetInventory(Inventory());
    CampaignReferenceBinding::Bind(owner, 0x8a986, item);
    savedItem = item.GetChangeForm();
    savedItem.count = data.count;
    REQUIRE(savedItem.count == 15);
    SqliteDatabase database(files.File());
    REQUIRE(database.Upsert({ owner.GetChangeForm(), savedNpc, savedItem }) ==
            3);
  }
  auto storage =
    std::make_shared<Storage>(std::make_shared<SqliteDatabase>(files.File()),
                              spdlog::default_logger(), "sqlite", 1);
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  restarted.AttachSaveStorage(storage);
  auto& owner = restarted.worldState.GetFormAt<MpActor>(ownerId);
  auto& npc = restarted.worldState.GetFormAt<MpActor>(npcId);
  auto& item = restarted.worldState.GetFormAt<MpObjectReference>(itemId);
  REQUIRE(MpChangeForm::ToJson(npc.GetChangeForm()) ==
          MpChangeForm::ToJson(savedNpc));
  REQUIRE(MpChangeForm::ToJson(item.GetChangeForm()) ==
          MpChangeForm::ToJson(savedItem));
  REQUIRE_FALSE(npc.IsRespawning());
  REQUIRE(npc.GetFactions(-128, 127).empty());
  REQUIRE(npc.GetChangeForm().actorValues.health == 314);
  REQUIRE(npc.GetInventory().GetItemCount(0xf) == 19);
  REQUIRE(CampaignReferenceBinding::Context(npc).first == &owner);
  REQUIRE(&CampaignReferenceBinding::Resolve(owner, 0x8a986) == &item);
}

TEST_CASE("Malformed private reference saves are rejected before registration",
          "[SQLite][Quest][ReferenceLifecycle]")
{
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  constexpr uint32_t ownerId = 0xff002001, npcId = 0xff002010;
  server.CreateActor(ownerId, { 1, 2, 3 }, 0, 0x3c);
  auto& owner = server.worldState.GetFormAt<MpActor>(ownerId);
  owner.SetWorldInstance("campaign:" + std::to_string(ownerId) + ":fixture");
  server.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
      FormCallbacks::DoNothing(), 0x13bbd),
    npcId);
  auto& npc = server.worldState.GetFormAt<MpActor>(npcId);
  npc.SetWorldInstance(owner.GetWorldInstance());
  npc.SetInventory(Inventory());
  CampaignReferenceBinding::Bind(owner, 0x1a677, npc);
  auto saved = npc.GetChangeForm();
  SECTION("wrong base")
  {
    saved.baseDesc = { 0x7, "Skyrim.esm" };
  }
  SECTION("shared world")
  {
    saved.worldInstance = "";
  }
  SECTION("player identity")
  {
    saved.profileId = 17;
  }
  SECTION("inventory not initialized")
  {
    saved.baseContainerAdded = false;
  }
  SECTION("pending loot timer")
  {
    saved.nextRelootDatetime = 1;
  }
  SECTION("active effect")
  {
    saved.activeMagicEffects.Add(espm::ActorValue::HealRate,
                                 ActiveMagicEffectsMap::Entry{});
  }
  SECTION("wrong record kind")
  {
    saved.recType = MpChangeForm::REFR;
  }
  SECTION("invalid identity")
  {
    auto binding = nlohmann::json::parse(
      saved.dynamicFields.GetValueDump(CampaignReferenceBinding::kProperty));
    binding["identity"] = "invalid";
    saved.dynamicFields.SetValueDump(CampaignReferenceBinding::kProperty,
                                     binding.dump());
  }
  PartOne restarted;
  restarted.AttachEspm(&GetEspmLoader());
  REQUIRE_THROWS(
    restarted.worldState.LoadChangeForm(saved, FormCallbacks::DoNothing()));
  REQUIRE_FALSE(restarted.worldState.LookupFormByIdNoLoad(npcId));
}

#if defined(__linux__)
TEST_CASE("SQLite discards an interrupted uncommitted transaction",
          "[SQLite][save]")
{
  SqliteFiles files;
  {
    SqliteDatabase seed(files.File());
    seed.Upsert({ SavedForm(1, 1), SavedForm(2, 1) });
  }
  const auto child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    try {
      // Force dirty pages into the WAL without a commit marker. The driver
      // must recover the preceding committed snapshot when it reopens.
      RawDatabase unfinished(files.File());
      unfinished.Execute("PRAGMA cache_size=2; BEGIN IMMEDIATE; UPDATE "
                         "change_forms SET body='incomplete'; INSERT INTO "
                         "change_forms VALUES('3',zeroblob(262144))");
      kill(getpid(), SIGKILL);
    } catch (...) {
      _exit(90);
    }
    _exit(91);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGKILL);
  REQUIRE(std::filesystem::file_size(files.File() + "-wal") > 4096);
  SqliteDatabase reopened(files.File());
  const auto restored = Read(reopened);
  REQUIRE(restored.size() == 2);
  REQUIRE(restored.at("1") == MpChangeForm::ToJson(SavedForm(1, 1)));
  REQUIRE(restored.at("2") == MpChangeForm::ToJson(SavedForm(2, 1)));
}

TEST_CASE("SQLite committed saves survive abrupt process death",
          "[SQLite][save]")
{
  SqliteFiles files;
  {
    SqliteDatabase seed(files.File());
    seed.Upsert({ SavedForm(1, 1), SavedForm(2, 1) });
  }
  const auto child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    try {
      SqliteDatabase writer(files.File());
      writer.Upsert({ SavedForm(1, 2), SavedForm(2, 2) });
      // No C++ destructors, SQLite close or clean checkpoint after this
      // commit.
      kill(getpid(), SIGKILL);
    } catch (...) {
      _exit(90);
    }
    _exit(91);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGKILL);
  REQUIRE(std::filesystem::exists(files.File() + "-wal"));
  SqliteDatabase reopened(files.File());
  REQUIRE(Read(reopened).at("1") == MpChangeForm::ToJson(SavedForm(1, 2)));
  REQUIRE(Read(reopened).at("2") == MpChangeForm::ToJson(SavedForm(2, 2)));
}
#endif
