#pragma once
#include "MpChangeForms.h"
#include <database_drivers/IDatabase.h>
#include <memory>

// One Upsert is one durable, all-or-nothing batch. Like the other IDatabase
// implementations, calls are serialized by AsyncSaveStorage. Separate database
// connections may read concurrently through SQLite WAL snapshots.
class SqliteDatabase
  : public Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
public:
  explicit SqliteDatabase(const std::string& file);
  ~SqliteDatabase() override;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  void Iterate(const IterateCallback& callback,
               std::optional<std::vector<FormDesc>> filter) override;

private:
  std::vector<std::optional<MpChangeForm>>&& UpsertImpl(
    std::vector<std::optional<MpChangeForm>>&& forms,
    size_t& outNumUpserted) override;
  struct Impl;
  std::unique_ptr<Impl> pImpl;
};
