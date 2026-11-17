#include "QuestJournal.h"
#include "SaveReceipt.h"
#include "TestUtils.hpp"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <utility>

extern espm::Loader& GetEspmLoader();

namespace {
using Json = nlohmann::json;
using Forms = std::vector<std::optional<MpChangeForm>>;
using Receipt = std::shared_ptr<const SaveReceipt>;

struct ControlledStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  struct Submission
  {
    Forms forms;
    UpsertResultCallback complete;
  };
  bool atomic = true, results = true, automatic = true;
  bool successBeforeSubmitError = false;
  mutable std::function<void()> onResultsCapability;
  std::vector<Submission> submissions;
  std::exception_ptr tickError, submitError;
  uint32_t finished = 0;
  bool SupportsAtomicBatches() const noexcept override { return atomic; }
  bool SupportsUpsertResults() const noexcept override
  {
    if (auto callback = std::exchange(onResultsCapability, {}))
      callback();
    return results;
  }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  void Upsert(Forms&& forms, const UpsertCallback& callback) override
  {
    UpsertWithResult(std::move(forms), [callback](std::exception_ptr error) {
      if (error)
        std::rethrow_exception(error);
      callback();
    });
  }
  void UpsertWithResult(Forms&& forms,
                        const UpsertResultCallback& callback) override
  {
    submissions.push_back({ std::move(forms), callback });
    if (submitError) {
      if (successBeforeSubmitError)
        Complete(submissions.size() - 1);
      std::rethrow_exception(std::exchange(submitError, {}));
    }
    if (automatic)
      Complete(submissions.size() - 1);
  }
  void Complete(size_t index, std::exception_ptr error = {})
  {
    // Keep callbacks available to inject duplicates and stale results.
    auto callback = submissions.at(index).complete;
    callback(error);
    if (!error)
      ++finished;
  }
  uint32_t GetNumFinishedUpserts() const override { return finished; }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override
  {
    if (tickError)
      std::rethrow_exception(std::exchange(tickError, {}));
  }
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "controlled-receipts";
    return name;
  }
};

Json Serialized(const Forms& forms)
{
  auto result = Json::array();
  for (const auto& form : forms)
    result.push_back(form ? MpChangeForm::ToJson(*form) : Json(nullptr));
  return result;
}

const MpChangeForm& Find(const Forms& forms, const FormDesc& id)
{
  for (const auto& form : forms)
    if (form && form->formDesc == id)
      return *form;
  throw std::runtime_error("Expected saved receipt participant");
}

Json Journal(const MpChangeForm& form)
{
  return Json::parse(form.dynamicFields.GetValueDump(QuestJournal::kProperty));
}

struct ReceiptWorld
{
  PartOne server;
  std::shared_ptr<ControlledStorage> storage =
    std::make_shared<ControlledStorage>();
  MpActor *owner, *guest;
  MpObjectReference* chest;
  ReceiptWorld()
  {
    server.AttachEspm(&GetEspmLoader());
    server.AttachSaveStorage(storage);
    server.CreateActor(0xff003000, { 1, 2, 3 }, 0, 0x3c);
    server.CreateActor(0xff003001, { 1, 2, 3 }, 0, 0x3c);
    owner = &server.worldState.GetFormAt<MpActor>(0xff003000);
    guest = &server.worldState.GetFormAt<MpActor>(0xff003001);
    for (auto* actor : { owner, guest })
      actor->SetPropertyValueDump(QuestJournal::kEnabled,
                                  "[\"4e50d:Skyrim.esm\"]", false, false);
    server.worldState.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ { 1, 2, 3 }, { 0, 0, 0 }, FormDesc::Tamriel() },
        FormCallbacks::DoNothing(), 0x10cd5b, "CONT"),
      0xff003002);
    chest = &server.worldState.GetFormAt<MpObjectReference>(0xff003002);
    const auto phase =
      "campaign:" + std::to_string(owner->GetFormId()) + ":receipt";
    for (auto* ref : std::vector<MpObjectReference*>{ owner, guest, chest }) {
      ref->SetWorldInstance(phase);
      ref->SetInventory(Inventory().AddItem(0xf, 10));
    }
    server.worldState.Tick();
    storage->submissions.clear();
    storage->automatic = false;
  }
  void Tick() { server.worldState.Tick(); }
  void Stage()
  {
    QuestJournal::Apply(
      *guest, { { "quest", 0x4e50d }, { "op", "setStage" }, { "index", 10 } });
  }
  void Transfer(uint32_t amount)
  {
    QuestJournal::EditInventory(
      *guest, *owner, [amount](Inventory& inv) { inv.AddItem(0xf, amount); });
    QuestJournal::EditInventory(*guest, *chest, [amount](Inventory& inv) {
      inv.RemoveItems({ { 0xf, amount } });
    });
  }
  Receipt Commit(uint32_t amount, int marker)
  {
    return QuestJournal::AtomicDurable(*guest, [&] {
      Stage();
      Transfer(amount);
      QuestJournal::SetScriptState(*guest, { { "marker", marker } });
    });
  }
};
}

TEST_CASE("Durable campaign receipts identify immutable ordered save batches",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto ownerId = f.owner->GetChangeForm().formDesc;
  const auto guestId = f.guest->GetChangeForm().formDesc;
  const auto chestId = f.chest->GetChangeForm().formDesc;
  f.guest->SetInventory(Inventory().AddItem(0xf, 20));
  const auto first = f.Commit(6, 1);
  REQUIRE(first->GetState() == SaveReceipt::State::Pending);
  REQUIRE(first->GetSequence() > 0);
  REQUIRE(first->GetCampaign() == ownerId);
  REQUIRE(first->GetJournalRevision() ==
          QuestJournal::Snapshot(*f.owner).at("revision").get<uint64_t>());
  const auto second = f.Commit(1, 2);
  REQUIRE(second->GetSequence() > first->GetSequence());
  REQUIRE(second->GetJournalRevision() > first->GetJournalRevision());
  // Ordinary writes after sealing must not rewrite either receipt snapshot.
  f.owner->SetInventory(Inventory().AddItem(0xf, 23));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto& firstForms = f.storage->submissions[0].forms;
  REQUIRE(Find(firstForms, ownerId).inv.GetItemCount(0xf) == 16);
  REQUIRE(Find(firstForms, guestId).inv.GetItemCount(0xf) == 20);
  REQUIRE(Find(firstForms, chestId).inv.GetItemCount(0xf) == 4);
  REQUIRE(Journal(Find(firstForms, ownerId))["vm"]["marker"] == 1);
  REQUIRE(first->GetAttempts() == 1);
  REQUIRE(second->GetAttempts() == 0);
  REQUIRE(first->GetState() == SaveReceipt::State::Pending);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Durable);
  REQUIRE(second->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(
    Find(f.storage->submissions[1].forms, ownerId).inv.GetItemCount(0xf) ==
    17);
  REQUIRE(Journal(Find(f.storage->submissions[1].forms,
                       ownerId))["vm"]["marker"] == 2);
  f.storage->Complete(0);
  f.storage->Complete(0, std::make_exception_ptr(std::runtime_error("stale")));
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Durable);
  REQUIRE(second->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(second->GetState() == SaveReceipt::State::Durable);
  REQUIRE(f.storage->submissions.size() == 3);
  REQUIRE(
    Find(f.storage->submissions[2].forms, ownerId).inv.GetItemCount(0xf) ==
    23);
  f.storage->Complete(2);
}

TEST_CASE("Durable campaign saves retain writes from the storage preflight",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const bool editsParticipant = GENERATE(false, true);
  auto& target = editsParticipant ? *f.owner : *f.guest;
  bool called = false;
  std::exception_ptr callbackError;
  // Arm the storage callback after staged rows reach the projection barrier.
  f.server.worldState.SetCampaignVisibilityCallbacks(
    [&](MpActor&) {
      f.storage->onResultsCapability = [&] {
        called = true;
        try {
          target.SetInventory(Inventory().AddItem(0xf, 27));
        } catch (...) {
          callbackError = std::current_exception();
        }
      };
    },
    [](MpActor&, bool) {});
  const auto initialJournal =
    f.owner->GetDynamicFields().GetValueDump(QuestJournal::kProperty);
  auto commit = [&] {
    return QuestJournal::AtomicDurable(*f.guest, [&] {
      f.Stage();
      f.Transfer(1);
      QuestJournal::EditFactions(*f.guest, *f.owner, [&](auto& factions) {
        factions = {
          { FormDesc::FromFormId(0x5c84d, f.server.worldState.espmFiles), 4 }
        };
      });
    });
  };
  if (editsParticipant) {
    REQUIRE_THROWS(commit());
    REQUIRE(f.owner->GetDynamicFields().GetValueDump(
              QuestJournal::kProperty) == initialJournal);
    REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
  } else {
    REQUIRE(commit()->GetState() == SaveReceipt::State::Pending);
    REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 9);
  }
  REQUIRE(called);
  REQUIRE_FALSE(callbackError);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto& row = Find(f.storage->submissions.front().forms,
                         target.GetChangeForm().formDesc);
  REQUIRE(row.inv.GetItemCount(0xf) == 27);
}

TEST_CASE("Failed receipt attempts retry exact snapshots before later commits",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto first = f.Commit(2, 1);
  const auto second = f.Commit(3, 2);
  f.Tick();
  const auto original = Serialized(f.storage->submissions[0].forms);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("disk failed")));
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Pending);
  REQUIRE(first->GetAttempts() == 2);
  REQUIRE(first->GetLastError());
  REQUIRE_THROWS_WITH(std::rethrow_exception(first->GetLastError()),
                      "disk failed");
  REQUIRE_FALSE(second->GetLastError());
  REQUIRE(second->GetAttempts() == 0);
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == original);
  // A late result from the failed attempt cannot finish the active retry.
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(
    1, std::make_exception_ptr(std::runtime_error("disk still failed")));
  f.Tick();
  REQUIRE(first->GetAttempts() == 3);
  REQUIRE(Serialized(f.storage->submissions[2].forms) == original);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 15);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 5);
  f.storage->Complete(2);
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Durable);
  REQUIRE(second->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 4);
  f.storage->Complete(3);
  f.Tick();
  REQUIRE(second->GetState() == SaveReceipt::State::Durable);
}

TEST_CASE("Unrelated storage errors cannot release an active receipt barrier",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto first = f.Commit(1, 1);
  const auto second = f.Commit(1, 2);
  f.Tick();
  f.storage->tickError =
    std::make_exception_ptr(std::runtime_error("read failed"));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  REQUIRE(first->GetState() == SaveReceipt::State::Pending);
  REQUIRE(first->GetAttempts() == 1);
  REQUIRE(second->GetAttempts() == 0);
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(first->GetState() == SaveReceipt::State::Durable);
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(1);
}

TEST_CASE("Submission exceptions retain the immutable durable batch",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto receipt = f.Commit(1, 1);
  f.storage->submitError =
    std::make_exception_ptr(std::runtime_error("queue failed"));
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  REQUIRE(receipt->GetLastError());
  REQUIRE(f.storage->submissions.size() == 1);
  const auto original = Serialized(f.storage->submissions[0].forms);
  f.owner->SetInventory(Inventory().AddItem(0xf, 50));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == original);
  f.storage->Complete(0);
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
}

TEST_CASE(
  "Receipt retries do not reconstruct destroyed or reused participants",
  "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto chestId = f.chest->GetFormId();
  const auto receipt = f.Commit(2, 1);
  f.Tick();
  const auto original = Serialized(f.storage->submissions[0].forms);
  f.server.worldState.DestroyForm(chestId);
  SECTION("participant no longer exists")
  {
  }
  SECTION("form ID is reused by a different actor")
  {
    f.server.CreateActor(chestId, { 8, 9, 10 }, 0, 0x3c);
    f.server.worldState.GetFormAt<MpActor>(chestId).SetInventory(
      Inventory().AddItem(0xf, 77));
  }
  f.storage->Complete(0, std::make_exception_ptr(std::runtime_error("retry")));
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 2);
  REQUIRE(Serialized(f.storage->submissions[1].forms) == original);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
}

TEST_CASE(
  "Journal-only and no-op durable transactions still await owner saves",
  "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto ownerId = f.owner->GetChangeForm().formDesc;
  const auto receipt = QuestJournal::AtomicDurable(*f.guest, [&] {
    SECTION("journal and VM only")
    {
      f.Stage();
      QuestJournal::SetScriptState(*f.guest, { { "awaitToken", "one" } });
    }
    SECTION("no changes")
    {
    }
  });
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  REQUIRE(
    Find(f.storage->submissions[0].forms, ownerId).inv.GetItemCount(0xf) ==
    10);
  REQUIRE(receipt->GetJournalRevision() ==
          QuestJournal::Snapshot(*f.owner).at("revision").get<uint64_t>());
  f.storage->Complete(0);
  REQUIRE(receipt->GetState() == SaveReceipt::State::Pending);
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
}

TEST_CASE(
  "An inline success remains authoritative when submission then throws",
  "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto receipt = f.Commit(1, 1);
  f.storage->successBeforeSubmitError = true;
  f.storage->submitError =
    std::make_exception_ptr(std::runtime_error("throw after success"));
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  REQUIRE(receipt->GetAttempts() == 1);
  REQUIRE_FALSE(receipt->GetLastError());
  REQUIRE(f.storage->submissions.size() == 1);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto next = f.Commit(1, 2);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(
    0, std::make_exception_ptr(std::runtime_error("stale failure")));
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  REQUIRE(next->GetState() == SaveReceipt::State::Pending);
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(1);
  f.Tick();
  REQUIRE(next->GetState() == SaveReceipt::State::Durable);
}

TEST_CASE("A full receipt queue rejects execution before publishing more work",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  std::vector<Receipt> receipts;
  for (size_t i = 0; i < 256; ++i)
    receipts.push_back(QuestJournal::AtomicDurable(*f.guest, [] {}));
  const auto before = QuestJournal::Snapshot(*f.owner);
  bool ran = false;
  REQUIRE_THROWS(QuestJournal::AtomicDurable(*f.guest, [&] {
    ran = true;
    f.Stage();
    f.Transfer(1);
  }));
  REQUIRE_FALSE(ran);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.storage->submissions.empty());
  REQUIRE(receipts.back()->GetState() == SaveReceipt::State::Pending);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  f.storage->Complete(0);
  REQUIRE(receipts.front()->GetState() == SaveReceipt::State::Pending);
  f.Tick();
  REQUIRE(receipts.front()->GetState() == SaveReceipt::State::Durable);
  REQUIRE(f.storage->submissions.size() == 2);
  const auto admitted = QuestJournal::AtomicDurable(*f.guest, [] {});
  REQUIRE(admitted->GetState() == SaveReceipt::State::Pending);
  REQUIRE(admitted->GetSequence() > receipts.back()->GetSequence());
  REQUIRE(admitted->GetAttempts() == 0);
}

TEST_CASE("Durable admission and execution failures publish no saved work",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  bool ran = false;
  SECTION("non-atomic storage")
  {
    f.storage->atomic = false;
  }
  SECTION("no correlated result support")
  {
    f.storage->results = false;
  }
  SECTION("no storage")
  {
    f.server.worldState.AttachSaveStorage(nullptr);
  }
  REQUIRE_THROWS(QuestJournal::AtomicDurable(*f.guest, [&] {
    ran = true;
    f.Stage();
    f.Transfer(1);
  }));
  REQUIRE_FALSE(ran);
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
}

TEST_CASE("A throwing durable transaction does not seal a partial reward",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto before = QuestJournal::Snapshot(*f.owner);
  REQUIRE_THROWS(QuestJournal::AtomicDurable(*f.guest, [&] {
    f.Stage();
    f.Transfer(1);
    throw std::runtime_error("fragment failed");
  }));
  REQUIRE(QuestJournal::Snapshot(*f.owner) == before);
  REQUIRE(f.owner->GetInventory().GetItemCount(0xf) == 10);
  REQUIRE(f.chest->GetInventory().GetItemCount(0xf) == 10);
  f.Tick();
  REQUIRE(f.storage->submissions.empty());
}

TEST_CASE("Storage preflight pins the guest and host before executing a quest",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto id =
    GENERATE(false, true) ? f.owner->GetFormId() : f.guest->GetFormId();
  bool executed = false, pinned = false;
  std::shared_ptr<MpActor> removed;
  std::exception_ptr callbackError;
  f.storage->onResultsCapability = [&] {
    try {
      f.server.worldState.DestroyForm(id, &removed);
      pinned = removed.use_count() > 1;
      f.server.CreateActor(id, { 7, 8, 9 }, 0, 0x3c);
    } catch (...) {
      callbackError = std::current_exception();
    }
  };
  REQUIRE_THROWS(
    QuestJournal::AtomicDurable(*f.guest, [&] { executed = true; }));
  REQUIRE_FALSE(callbackError);
  REQUIRE(pinned);
  REQUIRE_FALSE(executed);
  REQUIRE(f.server.worldState.LookupFormByIdNoLoad(id) != removed);
  REQUIRE(f.storage->submissions.empty());
}

TEST_CASE(
  "Clear during save capability checks never submits the abandoned batch",
  "[Quest][CampaignInventory][SaveReceipt][Clear]")
{
  ReceiptWorld f;
  const auto receipt = f.Commit(1, 1);
  const auto queued = f.Commit(1, 2);
  f.storage->onResultsCapability = [&] { f.server.worldState.Clear(); };
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(queued->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(receipt->GetAttempts() == 0);
  REQUIRE(f.storage->submissions.empty());
  const uint32_t id = 0xff003100;
  f.server.CreateActor(id, { 3, 4, 5 }, 0, 0x3c);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  const auto& forms = f.storage->submissions.front().forms;
  REQUIRE(std::count_if(forms.begin(), forms.end(), [](const auto& form) {
            return form.has_value();
          }) == 1);
  REQUIRE(Find(forms, FormDesc::FromFormId(id, f.server.worldState.espmFiles))
            .position == NiPoint3(3, 4, 5));
}

TEST_CASE(
  "Pending receipts prevent storage replacement and survive same attachment",
  "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  auto replacement = std::make_shared<ControlledStorage>();
  const auto receipt = f.Commit(1, 1);
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(f.storage));
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(replacement));
  f.Tick();
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(replacement));
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Durable);
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(replacement));
}

TEST_CASE(
  "Clear abandons receipts while an old submission remains a write barrier",
  "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  const auto receipt = f.Commit(1, 1);
  const auto queued = f.Commit(1, 2);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  REQUIRE_NOTHROW(f.server.worldState.Clear());
  REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(queued->GetState() == SaveReceipt::State::Abandoned);
  f.server.CreateActor(0xff003100, { 3, 4, 5 }, 0, 0x3c);
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  SECTION("old write succeeds")
  {
    f.storage->Complete(0);
  }
  SECTION("old write fails without retry")
  {
    f.storage->Complete(
      0, std::make_exception_ptr(std::runtime_error("old failed")));
  }
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(queued->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(f.storage->submissions.size() == 2);
  const auto& newActor = f.server.worldState.GetFormAt<MpActor>(0xff003100);
  REQUIRE(
    Find(f.storage->submissions[1].forms, newActor.GetChangeForm().formDesc)
      .position == NiPoint3(3, 4, 5));
  f.storage->Complete(0);
  f.Tick();
  REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  REQUIRE(f.storage->submissions.size() == 2);
  f.storage->Complete(1);
}

TEST_CASE("Clear removes old world indexes and timers before actor ID reuse",
          "[WorldState][SaveReceipt][Clear]")
{
  int oldRegular = 0, oldEffect = 0, newRegular = 0, newEffect = 0;
  PartOne server;
  auto& world = server.worldState;
  constexpr uint32_t reusedId = 0xff003200, oldSecondId = 0xff003201;
  server.CreateActor(reusedId, { 1, 2, 3 }, 0, 0x3c, ProfileId(42));
  server.CreateActor(oldSecondId, { 1, 2, 3 }, 0, 0x3c, ProfileId(42));
  const std::string property = "private.indexed.clearTest";
  const auto propertyKey =
    world.MakePrivateIndexedPropertyMapKey(property, "\"old\"");
  int oldSecondIdx = -1;
  {
    auto& first = world.GetFormAt<MpActor>(reusedId);
    auto& second = world.GetFormAt<MpActor>(oldSecondId);
    first.RegisterPrivateIndexedProperty(property, "\"old\"");
    second.RegisterPrivateIndexedProperty(property, "\"old\"");
    oldSecondIdx = second.GetIdx();
    REQUIRE(oldSecondIdx > 0);
    REQUIRE(world.LookupFormByIdx(oldSecondIdx) == &second);
  }
  REQUIRE(world.GetActorsByProfileId(42) ==
          std::set<uint32_t>({ reusedId, oldSecondId }));
  REQUIRE(world.GetActorsByPrivateIndexedProperty(propertyKey) ==
          std::set<uint32_t>({ reusedId, oldSecondId }));
  world.hosters[oldSecondId] = reusedId;
  world.activationChildsByActivationParent[reusedId][oldSecondId] = 1.0f;
  world.lastMovUpdateByIdx.resize(oldSecondIdx + 1);
  world.lastMovUpdateByIdx[oldSecondIdx] = std::chrono::system_clock::now();
  world.SetTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++oldRegular;
  });
  world.SetEffectTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++oldEffect;
  });

  REQUIRE_NOTHROW(world.Clear());
  REQUIRE(world.actorIdByProfileId.empty());
  REQUIRE(world.actorIdByPrivateIndexedProperty.empty());
  REQUIRE(world.hosters.empty());
  REQUIRE(world.activationChildsByActivationParent.empty());
  REQUIRE(world.lastMovUpdateByIdx.empty());
  REQUIRE(oldRegular == 0);
  REQUIRE(oldEffect == 0);

  server.CreateActor(reusedId, { 7, 8, 9 }, 0, 0x3c, ProfileId(99));
  auto& replacement = world.GetFormAt<MpActor>(reusedId);
  REQUIRE(replacement.GetIdx() == 0);
  REQUIRE(world.LookupFormByIdx(0) == &replacement);
  REQUIRE(world.LookupFormByIdx(oldSecondIdx) == nullptr);
  REQUIRE_FALSE(world.LookupFormByIdNoLoad(oldSecondId));
  REQUIRE(world.GetActorsByProfileId(42).empty());
  REQUIRE(world.GetActorsByProfileId(99) == std::set<uint32_t>{ reusedId });
  REQUIRE(world.GetActorsByPrivateIndexedProperty(propertyKey).empty());
  REQUIRE(world.hosters.empty());
  world.Tick();
  REQUIRE(oldRegular == 0);
  REQUIRE(oldEffect == 0);

  world.SetTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++newRegular;
  });
  world.SetEffectTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++newEffect;
  });
  world.Tick();
  REQUIRE(newRegular == 1);
  REQUIRE(newEffect == 1);
  world.Tick();
  REQUIRE(newRegular == 1);
  REQUIRE(newEffect == 1);
  REQUIRE(oldRegular == 0);
  REQUIRE(oldEffect == 0);
}

TEST_CASE("Ordinary dirty work also prevents switching persistence backends",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  ReceiptWorld f;
  auto replacement = std::make_shared<ControlledStorage>();
  f.owner->SetInventory(Inventory().AddItem(0xf, 11));
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(f.storage));
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(replacement));
  f.Tick();
  REQUIRE(f.storage->submissions.size() == 1);
  REQUIRE_THROWS(f.server.worldState.AttachSaveStorage(replacement));
  f.storage->Complete(0);
  f.Tick();
  REQUIRE_NOTHROW(f.server.worldState.AttachSaveStorage(replacement));
}

TEST_CASE("Clearing unsent work and destroying a world abandon its receipts",
          "[Quest][CampaignInventory][SaveReceipt]")
{
  Receipt receipt;
  std::shared_ptr<ControlledStorage> storage;
  bool submitted = false;
  {
    ReceiptWorld f;
    storage = f.storage;
    receipt = f.Commit(1, 1);
    SECTION("clear before submission")
    {
      f.server.worldState.Clear();
      f.Tick();
      REQUIRE(storage->submissions.empty());
      REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
    }
    SECTION("destroy with a queued receipt")
    {
    }
    SECTION("destroy with an in-flight receipt")
    {
      f.Tick();
      submitted = true;
    }
  }
  REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  if (submitted) {
    REQUIRE_NOTHROW(storage->Complete(0));
    REQUIRE(receipt->GetState() == SaveReceipt::State::Abandoned);
  }
}
