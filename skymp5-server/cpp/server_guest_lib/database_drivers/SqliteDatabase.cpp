#include "SqliteDatabase.h"
#include <climits>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <save_storages/AsyncSaveStorage.h>
#include <sqlite3.h>
#include <stdexcept>
#include <unordered_set>

// Includes the upstream WAL-reset fix. The pinned vcpkg port is 3.52.0.
static_assert(SQLITE_VERSION_NUMBER >= 3051003);

namespace {
constexpr int kApplicationId = 0x534b4d50; // SKMP
constexpr int kSchemaVersion = 1;
constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;
using SaveStorage =
  Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>;

void Check(sqlite3* db, int code, const char* operation)
{
  if (code != SQLITE_OK)
    throw std::runtime_error(std::string("SQLite ") + operation + ": " +
                             (db ? sqlite3_errmsg(db) : sqlite3_errstr(code)));
}
void Execute(sqlite3* db, const char* sql)
{
  Check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr), "execute");
}

struct Statement
{
  sqlite3* db;
  sqlite3_stmt* value = nullptr;
  Statement(sqlite3* db_, const char* sql)
    : db(db_)
  {
    Check(db, sqlite3_prepare_v2(db, sql, -1, &value, nullptr), "prepare");
  }
  ~Statement() { sqlite3_finalize(value); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  void Bind(int index, const std::string& text)
  {
    Check(db,
          sqlite3_bind_text64(value, index, text.data(), text.size(),
                              SQLITE_TRANSIENT, SQLITE_UTF8),
          "bind");
  }
  bool Row()
  {
    const auto code = sqlite3_step(value);
    if (code == SQLITE_ROW)
      return true;
    if (code == SQLITE_DONE)
      return false;
    Check(db, code, "step");
    return false;
  }
  void Reset()
  {
    Check(db, sqlite3_reset(value), "reset");
    Check(db, sqlite3_clear_bindings(value), "clear bindings");
  }
  std::string Text(int column)
  {
    if (sqlite3_column_type(value, column) != SQLITE_TEXT)
      throw std::runtime_error("SQLite change form has a non-text column");
    const auto count = sqlite3_column_bytes(value, column);
    const auto* bytes = sqlite3_column_text(value, column);
    if (!bytes || count < 0 || static_cast<size_t>(count) > kMaxBodyBytes)
      throw std::runtime_error(
        "SQLite change form column is invalid or too large");
    return std::string(reinterpret_cast<const char*>(bytes), count);
  }
};

int Integer(sqlite3* db, const char* sql)
{
  Statement query(db, sql);
  if (!query.Row() || sqlite3_column_type(query.value, 0) != SQLITE_INTEGER)
    throw std::runtime_error("SQLite expected one integer result");
  const auto result = sqlite3_column_int64(query.value, 0);
  if (result < INT_MIN || result > INT_MAX || query.Row())
    throw std::runtime_error("SQLite invalid integer result");
  return static_cast<int>(result);
}

struct Transaction
{
  sqlite3* db;
  bool& healthy;
  bool committed = false;
  Transaction(sqlite3* db_, bool& healthy_, bool write)
    : db(db_)
    , healthy(healthy_)
  {
    if (!healthy || !sqlite3_get_autocommit(db))
      throw std::runtime_error(
        "SQLite connection has an unfinished transaction");
    Execute(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
  }
  ~Transaction()
  {
    if (!committed && !sqlite3_get_autocommit(db)) {
      const auto code =
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      if (code != SQLITE_OK || !sqlite3_get_autocommit(db))
        healthy = false;
    }
  }
  void Commit()
  {
    Execute(db, "COMMIT");
    committed = true;
  }
};
}

struct SqliteDatabase::Impl
{
  sqlite3* db = nullptr;
  bool healthy = true;
  ~Impl()
  {
    if (db)
      sqlite3_close_v2(db);
  }
};

SqliteDatabase::SqliteDatabase(const std::string& file)
  : pImpl(std::make_unique<Impl>())
{
  if (file.empty() || file.find('\0') != std::string::npos ||
      file == ":memory:")
    throw std::invalid_argument(
      "SQLite requires a persistent database filename");
  if (sqlite3_libversion_number() < 3051003)
    throw std::runtime_error(
      "SQLite runtime is too old for campaign WAL storage");
  const auto parent = std::filesystem::path(file).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  const auto openResult = sqlite3_open_v2(
    file.c_str(), &pImpl->db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
    nullptr);
  Check(pImpl->db, openResult, "open");
  auto* db = pImpl->db;
  Check(db, sqlite3_extended_result_codes(db, 1), "extended result codes");
  Check(db, sqlite3_busy_timeout(db, 5000), "busy timeout");
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH,
                static_cast<int>(kMaxBodyBytes + 1024));
  {
    Transaction schema(db, pImpl->healthy, true);
    const auto application = Integer(db, "PRAGMA application_id");
    const auto version = Integer(db, "PRAGMA user_version");
    if (application == 0 && version == 0 &&
        Integer(db, "SELECT count(*) FROM sqlite_schema") == 0) {
      Execute(db,
              "CREATE TABLE change_forms (form_desc TEXT PRIMARY KEY NOT "
              "NULL, body TEXT NOT NULL) WITHOUT ROWID");
      Execute(db, "PRAGMA application_id=0x534b4d50");
      Execute(db, "PRAGMA user_version=1");
    } else if (application != kApplicationId || version != kSchemaVersion) {
      throw std::runtime_error(
        "Unrecognized SQLite database or unsupported schema version");
    }
    Statement check(db, "SELECT form_desc, body FROM change_forms LIMIT 0");
    if (check.Row())
      throw std::runtime_error("Unexpected SQLite schema query result");
    schema.Commit();
  }
  Statement mode(db, "PRAGMA journal_mode=WAL");
  if (!mode.Row() || mode.Text(0) != "wal" || mode.Row())
    throw std::runtime_error("SQLite database does not support WAL mode");
  Execute(db, "PRAGMA synchronous=FULL");
  Execute(db, "PRAGMA fullfsync=ON");
  Execute(db, "PRAGMA trusted_schema=OFF");
  if (Integer(db, "PRAGMA synchronous") != 2)
    throw std::runtime_error("SQLite durable synchronization is unavailable");
}

SqliteDatabase::~SqliteDatabase() = default;

std::vector<std::optional<MpChangeForm>>&& SqliteDatabase::UpsertImpl(
  std::vector<std::optional<MpChangeForm>>&& forms, size_t& outNumUpserted)
{
  outNumUpserted = 0;
  try {
    auto* db = pImpl->db;
    Transaction transaction(db, pImpl->healthy, true);
    Statement insert(
      db,
      "INSERT INTO change_forms(form_desc,body) VALUES(?1,?2) ON "
      "CONFLICT(form_desc) DO UPDATE SET body=excluded.body");
    std::unordered_set<std::string> seen;
    size_t count = 0;
    for (const auto& form : forms) {
      if (!form)
        continue;
      const auto key = form->formDesc.ToString();
      if (!seen.insert(key).second)
        throw std::runtime_error(
          "Duplicate form identity in SQLite save batch");
      const auto body = MpChangeForm::ToJson(*form).dump();
      if (body.size() > kMaxBodyBytes)
        throw std::runtime_error("SQLite change form exceeds 32 MiB");
      insert.Bind(1, key);
      insert.Bind(2, body);
      if (insert.Row())
        throw std::runtime_error("Unexpected SQLite upsert row");
      insert.Reset();
      ++count;
    }
    transaction.Commit();
    outNumUpserted = count;
    return std::move(forms);
  } catch (const std::exception& error) {
    throw SaveStorage::UpsertFailedException(std::move(forms), error.what());
  }
}

void SqliteDatabase::Iterate(const IterateCallback& callback,
                             std::optional<std::vector<FormDesc>> filter)
{
  try {
    auto* db = pImpl->db;
    Transaction transaction(db, pImpl->healthy, false);
    Statement select(
      db,
      filter ? "SELECT form_desc,body FROM change_forms WHERE form_desc=?1"
             : "SELECT form_desc,body FROM change_forms ORDER BY form_desc");
    simdjson::dom::parser parser;
    const auto read = [&] {
      while (select.Row()) {
        const auto key = select.Text(0);
        const auto body = select.Text(1);
        auto json = parser.parse(body).value();
        const auto form = MpChangeForm::JsonToChangeForm(json);
        if (form.formDesc.ToString() != key)
          throw std::runtime_error(
            "SQLite form key does not match its saved identity");
        callback(form);
      }
    };
    if (filter) {
      std::unordered_set<std::string> seen;
      for (const auto& form : *filter) {
        const auto key = form.ToString();
        if (!seen.insert(key).second)
          continue;
        select.Bind(1, key);
        read();
        select.Reset();
      }
    } else
      read();
    transaction.Commit();
  } catch (const std::exception& error) {
    throw SaveStorage::IterateFailedException(std::move(filter), error.what());
  }
}
