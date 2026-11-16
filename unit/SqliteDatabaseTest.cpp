#include "database_drivers/SqliteDatabase.h"
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
