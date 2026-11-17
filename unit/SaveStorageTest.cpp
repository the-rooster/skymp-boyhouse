#include "TestUtils.hpp"

#include "FormDesc.h"
#include "MpChangeForms.h"
#include "database_drivers/FileDatabase.h"
#include "database_drivers/ZipDatabase.h"
#include "save_storages/AsyncSaveStorage.h"
#include <condition_variable>
#include <filesystem>
#include <mutex>

namespace {
using ResultStorage = Viet::AsyncSaveStorage<int, int, int>;
using ResultStorageInterface = Viet::ISaveStorage<int, int, int>;

struct UnknownUpsertFailure
{
  int operation;
};

class GatedResultDatabase : public Viet::IDatabase<int, int, int>
{
public:
  bool SupportsAtomicBatches() const noexcept override { return true; }

  void Release()
  {
    std::lock_guard lock(mutex);
    released = true;
    condition.notify_all();
  }

  bool WaitForEntered(size_t count)
  {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return workers.size() >= count; });
  }

  bool WaitForFinished(size_t count)
  {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return finished >= count; });
  }

  std::vector<std::thread::id> Workers()
  {
    std::lock_guard lock(mutex);
    return workers;
  }

  std::vector<int> Rows()
  {
    std::lock_guard lock(mutex);
    return rows;
  }

  void Iterate(const IterateCallback& callback, std::optional<int>) override
  {
    for (const auto row : Rows())
      callback(row);
  }

protected:
  std::vector<std::optional<int>>&& UpsertImpl(
    std::vector<std::optional<int>>&& forms, size_t& count) override
  {
    const auto operation = forms.at(0).value();
    std::unique_lock lock(mutex);
    workers.push_back(std::this_thread::get_id());
    condition.notify_all();
    condition.wait(lock, [&] { return released; });
    if (operation >= 0)
      rows.push_back(operation);
    ++finished;
    condition.notify_all();
    if (operation == -2)
      throw UnknownUpsertFailure{ operation };
    if (operation < 0)
      throw std::runtime_error("database operation " +
                               std::to_string(operation));
    count = forms.size();
    return std::move(forms);
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  bool released = false;
  size_t finished = 0;
  std::vector<int> rows;
  std::vector<std::thread::id> workers;
};

struct ResultStorageFixture
{
  std::shared_ptr<GatedResultDatabase> database =
    std::make_shared<GatedResultDatabase>();
  ResultStorage storage{ database, nullptr, "gated-results", 1 };

  // Also unblock the worker when a REQUIRE fails before the explicit release.
  ~ResultStorageFixture() { database->Release(); }
};

class LegacyResultStorage : public ResultStorageInterface
{
public:
  size_t submissions = 0;
  void IterateSync(const IterateSyncCallback&) override {}
  void Upsert(std::vector<std::optional<int>>&&,
              const UpsertCallback&) override
  {
    ++submissions;
  }
  void Iterate(const IterateCallback&, const std::optional<int>&) override {}
  uint32_t GetNumFinishedUpserts() const override { return 0; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override {}
  bool GetRecycledChangeFormsBuffer(std::vector<std::optional<int>>&) override
  {
    return false;
  }
  const std::string& GetName() const override { return name; }

private:
  const std::string name = "legacy";
};

template <class Tick, class Finished>
void PollStorageResults(Tick tick, Finished finished)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!finished()) {
    tick();
    if (finished())
      return;
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Timed out waiting for storage results");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}

std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
MakeSaveStorageFile()
{
  auto directory = "unit/data";

  if (std::filesystem::exists(directory)) {
    std::filesystem::remove_all(directory);
  }

  return std::make_shared<
    Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>(
    std::make_shared<FileDatabase>(directory, spdlog::default_logger()),
    spdlog::default_logger(), "file");
}

std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
MakeSaveStorageZip()
{
  auto archivePath = "world.zip";

  if (std::filesystem::exists(archivePath)) {
    std::filesystem::remove(archivePath);
  }

  return std::make_shared<
    Viet::AsyncSaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>(
    std::make_shared<ZipDatabase>(archivePath, spdlog::default_logger()),
    spdlog::default_logger(), "zip");
}

std::vector<std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>>
MakeSaveStorages()
{
  return { MakeSaveStorageFile(), MakeSaveStorageZip() };
}

MpChangeForm CreateChangeForm(const char* descStr)
{
  MpChangeForm res;
  res.formDesc = FormDesc::FromString(descStr);
  return res;
}

void UpsertSync(
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>& st,
  std::vector<std::optional<MpChangeForm>> changeForms)
{
  bool finished = false;
  st.Upsert(std::move(changeForms), [&] { finished = true; });

  int i = 0;
  while (!finished) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    st.Tick();
    ++i;
    if (i > 2000)
      throw std::runtime_error("Timeout exceeded");
  }
}

void WaitForNextUpsert(
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>& st,
  WorldState& wst)
{
  uint32_t n = st.GetNumFinishedUpserts();

  int i = 0;
  while (n == st.GetNumFinishedUpserts()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    wst.Tick(); // should include st.Tick()

    ++i;
    if (i > 2000)
      throw std::runtime_error("Timeout exceeded");
  }
}

TEST_CASE("ChangeForm with spaces is saved correctly", "[save]")
{
  auto storagesToTest = MakeSaveStorages();

  for (const auto& st : storagesToTest) {
    SECTION("Testing with " + st->GetName())
    {
      MpChangeForm f1;
      f1.formDesc = { 1, "" };

      Appearance appearance;
      appearance.name = "La La La";
      appearance.weight = 0.1;

      f1.appearanceDump = appearance.ToJson();

      UpsertSync(*st, { f1 });

      auto res = Viet::ISaveStorageUtils::FindAllSync(*st);
      REQUIRE(res.size() == 1);
      REQUIRE(res[{ 1, "" }].appearanceDump == appearance.ToJson());
    }
  }
}

TEST_CASE("ChangeForm is saved correctly", "[save]")
{
  auto storagesToTest = MakeSaveStorages();

  for (const auto& st : storagesToTest) {
    SECTION("Testing with " + st->GetName())
    {
      MpChangeForm f1, f2;
      f1.formDesc = { 1, "" };
      f1.position = { 1, 2, 3 };
      f1.appearanceDump = "{}";
      f1.inv.AddItem(0xf, 1000);
      f1.equipment = Equipment();
      f1.actorValues.healthPercentage = 0.25f;
      f1.actorValues.magickaPercentage = 0.3f;
      f1.actorValues.staminaPercentage = 1.0f;
      f2.formDesc = { 2, "" };
      f2.position = { 2, 4, 6 };
      f2.isDisabled = true;
      f2.profileId = 10;
      f2.actorValues.healthPercentage = 0;
      f2.actorValues.magickaPercentage = 0;
      f2.actorValues.staminaPercentage = 0;
      UpsertSync(*st, { f1, f2 });
      Faction faction = Faction();
      faction.formDesc = FormDesc(13, "Skyrim.esm");
      faction.rank = 10;
      f2.factions = std::vector<Faction>();
      f2.factions.value().push_back(faction);
      UpsertSync(*st, { f1, f2 });

      auto res = Viet::ISaveStorageUtils::FindAllSync(*st);
      REQUIRE(res.size() == 2);
      REQUIRE(res[{ 1, "" }].position == NiPoint3(1, 2, 3));
      REQUIRE(res[{ 1, "" }].appearanceDump == "{}");
      REQUIRE(res[{ 1, "" }].equipment == Equipment());
      REQUIRE(res[{ 1, "" }].inv == Inventory().AddItem(0xf, 1000));
      REQUIRE(res[{ 1, "" }].isDisabled == false);
      REQUIRE(res[{ 1, "" }].profileId == -1);
      REQUIRE(res[{ 1, "" }].actorValues.healthPercentage == 0.25f);
      REQUIRE(res[{ 1, "" }].actorValues.magickaPercentage == 0.3f);
      REQUIRE(res[{ 1, "" }].actorValues.staminaPercentage == 1.0f);
      REQUIRE(res[{ 2, "" }].position == NiPoint3(2, 4, 6));
      REQUIRE(res[{ 2, "" }].isDisabled == true);
      REQUIRE(res[{ 2, "" }].profileId == 10);
      REQUIRE(res[{ 2, "" }].actorValues.healthPercentage == 0);
      REQUIRE(res[{ 2, "" }].actorValues.magickaPercentage == 0);
      REQUIRE(res[{ 2, "" }].actorValues.staminaPercentage == 0);
      REQUIRE(res[{ 2, "" }].factions.has_value());
      REQUIRE(res[{ 2, "" }].factions.value().size() == 1);
      REQUIRE(res[{ 2, "" }].factions.value()[0].formDesc.shortFormId == 13);
      REQUIRE(res[{ 2, "" }].factions.value()[0].formDesc.file ==
              "Skyrim.esm");
      REQUIRE(res[{ 2, "" }].factions.value()[0].rank == 10);
    }
  }
}

TEST_CASE("Upsert affects the number of change forms in the database in the "
          "correct way",
          "[save]")
{
  auto storagesToTest = MakeSaveStorages();

  for (const auto& st : storagesToTest) {
    SECTION("Testing with " + st->GetName())
    {
      REQUIRE(Viet::ISaveStorageUtils::CountSync(*st) == 0);

      UpsertSync(*st,
                 { CreateChangeForm("0"), CreateChangeForm("1"),
                   CreateChangeForm("2") });

      REQUIRE(Viet::ISaveStorageUtils::CountSync(*st) == 3);

      UpsertSync(*st,
                 { CreateChangeForm("0"), CreateChangeForm("1"),
                   CreateChangeForm("2"), CreateChangeForm("3") });

      REQUIRE(Viet::ISaveStorageUtils::CountSync(*st) == 4);
    }
  }
}

TEST_CASE("AttachSaveStorage forces loading", "[save]")
{
  auto storagesToTest = MakeSaveStorages();

  for (const auto& st : storagesToTest) {
    SECTION("Testing with " + st->GetName())
    {
      PartOne p;
      p.worldState.espmFiles = { "AaAaAa.esm" };
      p.worldState.AddForm(
        std::unique_ptr<MpObjectReference>(new MpObjectReference(
          LocationalData(), FormCallbacks::DoNothing(), 0xaaaa, "STAT")),
        0xee);

      auto& refr = p.worldState.GetFormAt<MpObjectReference>(0xee);
      REQUIRE(refr.GetPos() == NiPoint3(0, 0, 0));

      auto f = CreateChangeForm("ee:AaAaAa.esm");
      f.position = { 1, 1, 1 };
      f.baseDesc = FormDesc::FromString("aaaa:AaAaAa.esm");
      UpsertSync(*st, { f });
      p.AttachSaveStorage(st);

      REQUIRE(refr.GetPos() == NiPoint3(1, 1, 1));
    }
  }
}

TEST_CASE("Changes are transferred to SaveStorage", "[save]")
{
  auto storagesToTest = MakeSaveStorages();

  for (const auto& st : storagesToTest) {
    SECTION("Testing with " + st->GetName())
    {
      PartOne p;
      p.AttachSaveStorage(st);

      REQUIRE(Viet::ISaveStorageUtils::CountSync(*st) == 0);
      p.CreateActor(0xffaaaeee, { 1, 1, 1 }, 1, 0x3c);

      WaitForNextUpsert(*st, p.worldState);
      REQUIRE(Viet::ISaveStorageUtils::CountSync(*st) == 1);
    }
  }
}

TEST_CASE("Tracked upserts require explicit support and a callback",
          "[save][UpsertResults]")
{
  LegacyResultStorage legacy;
  REQUIRE_FALSE(legacy.SupportsUpsertResults());
  std::vector<std::optional<int>> untouched{ 1 };
  REQUIRE_THROWS(
    legacy.UpsertWithResult(std::move(untouched), [](std::exception_ptr) {}));
  REQUIRE(untouched == std::vector<std::optional<int>>{ 1 });
  REQUIRE(legacy.submissions == 0);

  ResultStorageFixture fixture;
  REQUIRE(fixture.storage.SupportsUpsertResults());
  std::vector<std::optional<int>> rejected{ 2 };
  REQUIRE_THROWS_AS(fixture.storage.UpsertWithResult(std::move(rejected), {}),
                    std::invalid_argument);
  REQUIRE(rejected == std::vector<std::optional<int>>{ 2 });
  REQUIRE(fixture.database->Workers().empty());
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 0);
}

TEST_CASE("Tracked upsert successes and failures remain correlated on Tick",
          "[save][UpsertResults]")
{
  ResultStorageFixture fixture;
  const auto callerThread = std::this_thread::get_id();
  std::vector<std::pair<int, std::exception_ptr>> results;
  std::vector<std::thread::id> callbackThreads;
  for (const auto operation : { -1, 2, -2, 4 }) {
    fixture.storage.UpsertWithResult(
      { operation }, [&, operation](std::exception_ptr error) {
        results.emplace_back(operation, error);
        callbackThreads.push_back(std::this_thread::get_id());
      });
  }
  REQUIRE(fixture.database->WaitForEntered(1));
  REQUIRE(results.empty());
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 0);
  REQUIRE(fixture.database->Workers().front() != callerThread);
  // The worker is blocked inside the database, while submission and Tick
  // remain available on the caller thread.
  REQUIRE_NOTHROW(fixture.storage.Tick());
  REQUIRE(results.empty());
  fixture.database->Release();
  REQUIRE(fixture.database->WaitForFinished(4));
  REQUIRE(results.empty()); // Database completion alone invokes no callbacks.
  PollStorageResults([&] { fixture.storage.Tick(); },
                     [&] { return results.size() == 4; });
  const std::vector<int> expected{ -1, 2, -2, 4 };
  for (size_t i = 0; i < results.size(); ++i) {
    REQUIRE(results[i].first == expected[i]);
    REQUIRE(callbackThreads[i] == callerThread);
  }
  REQUIRE(results[0].second);
  REQUIRE_THROWS_WITH(std::rethrow_exception(results[0].second),
                      "database operation -1");
  REQUIRE_FALSE(results[1].second);
  REQUIRE(results[2].second);
  try {
    std::rethrow_exception(results[2].second);
  } catch (const UnknownUpsertFailure& error) {
    REQUIRE(error.operation == -2);
  }
  REQUIRE_FALSE(results[3].second);
  REQUIRE(fixture.database->Rows() == std::vector<int>{ 2, 4 });
  std::vector<std::optional<int>> recycled;
  size_t recycledBatches = 0;
  while (fixture.storage.GetRecycledChangeFormsBuffer(recycled)) {
    REQUIRE(recycled.size() == 1);
    REQUIRE_FALSE(recycled.front().has_value());
    ++recycledBatches;
  }
  REQUIRE(recycledBatches == 2);
  for (const auto worker : fixture.database->Workers())
    REQUIRE(worker != callerThread);
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 4);
  for (size_t i = 0; i < 3; ++i)
    REQUIRE_NOTHROW(fixture.storage.Tick());
  REQUIRE(results.size() == 4);
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 4);
}

TEST_CASE("Untracked upsert errors keep the legacy exception path",
          "[save][UpsertResults]")
{
  ResultStorageFixture fixture;
  size_t failedCallback = 0, successCallback = 0, trackedCallback = 0;
  std::exception_ptr trackedResult;
  fixture.storage.Upsert({ -1 }, [&] { ++failedCallback; });
  fixture.storage.UpsertWithResult({ 2 }, [&](std::exception_ptr error) {
    ++trackedCallback;
    trackedResult = error;
  });
  fixture.storage.Upsert({ 3 }, [&] { ++successCallback; });
  fixture.database->Release();
  size_t globalFailures = 0;
  PollStorageResults(
    [&] {
      try {
        fixture.storage.Tick();
      } catch (const std::runtime_error& error) {
        REQUIRE(std::string(error.what()) == "database operation -1");
        ++globalFailures;
      }
    },
    [&] {
      return globalFailures == 1 && trackedCallback == 1 &&
        successCallback == 1;
    });
  REQUIRE(failedCallback == 0);
  REQUIRE_FALSE(trackedResult);
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 2);
  REQUIRE_NOTHROW(fixture.storage.Tick());
  REQUIRE(globalFailures == 1);
  REQUIRE(trackedCallback == 1);
  REQUIRE(successCallback == 1);
}

TEST_CASE("Throwing tracked result callbacks are consumed without starving "
          "later callbacks",
          "[save][UpsertResults]")
{
  ResultStorageFixture fixture;
  size_t throwingCalls = 0, laterCalls = 0;
  std::exception_ptr originalFailure, laterResult;
  fixture.storage.UpsertWithResult({ -2 }, [&](std::exception_ptr error) {
    originalFailure = error;
    ++throwingCalls;
    throw std::runtime_error("result callback threw");
  });
  fixture.storage.UpsertWithResult({ 4 }, [&](std::exception_ptr error) {
    laterResult = error;
    ++laterCalls;
  });
  fixture.database->Release();
  bool caught = false;
  PollStorageResults(
    [&] {
      try {
        fixture.storage.Tick();
      } catch (const std::runtime_error& error) {
        REQUIRE(std::string(error.what()) == "result callback threw");
        caught = true;
      }
    },
    [&] { return caught; });
  REQUIRE(throwingCalls == 1);
  REQUIRE(laterCalls == 0);
  REQUIRE(originalFailure);
  REQUIRE_THROWS_AS(std::rethrow_exception(originalFailure),
                    UnknownUpsertFailure);
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 0);
  PollStorageResults([&] { fixture.storage.Tick(); },
                     [&] { return laterCalls == 1; });
  REQUIRE_FALSE(laterResult);
  REQUIRE(fixture.storage.GetNumFinishedUpserts() == 1);
  REQUIRE_NOTHROW(fixture.storage.Tick());
  REQUIRE(throwingCalls == 1);
  REQUIRE(laterCalls == 1);
}
