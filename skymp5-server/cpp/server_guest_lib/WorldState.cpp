#include "WorldState.h"
#include "CampaignReferenceBinding.h"
#include "EvaluateTemplate.h"
#include "FormCallbacks.h"
#include "LeveledListUtils.h"
#include "LocationalDataUtils.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "MpObjectReference.h"
#include "QuestJournal.h"
#include "libespm/GroupUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include "papyrus-vm/Reader.h"
#include "papyrus-vm/Utils.h"
#include "script_classes/PapyrusClassesFactory.h"
#include "script_compatibility_policies/PapyrusCompatibilityPolicyFactory.h"
#include "script_storages/IScriptStorage.h"
#include <ScopedTask.h>
#include <Timer.h>
#include <algorithm>
#include <antigo/Context.h>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iterator>
#include <limits>
#include <optional>
#include <save_storages/AsyncSaveStorage.h> // UpsertFailedException
#include <save_storages/ISaveStorage.h>
#include <unordered_map>
#include <unordered_set>

namespace {

struct RelootTimeForTypesEntry
{
  std::string recordType;
  std::chrono::system_clock::duration time;

  Viet::Timer timer;
};
}

struct WorldState::Impl
{
  struct SaveBatch
  {
    std::vector<std::optional<MpChangeForm>> forms;
    std::shared_ptr<SaveReceipt> receipt;
    size_t receiptBytes = 0;
    uint64_t attempt = 0;
    bool submitted = false, tracked = false, resultReady = false;
    bool abandoned = false;
    std::exception_ptr result;
  };
  std::vector<std::optional<MpChangeForm>> changesByIdx;
  bool changesByIdxEmpty = true;
  std::list<std::shared_ptr<SaveBatch>> sealedSaves;
  size_t queuedReceiptBytes = 0;
  uint64_t nextSaveSequence = 0;
  bool saveTickActive = false, clearing = false;
  static constexpr size_t maxSaveBatches = 256;
  static constexpr size_t maxReceiptBytes = 128 * 1024 * 1024;

  std::shared_ptr<
    Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
    saveStorage;
  std::shared_ptr<IScriptStorage> scriptStorage;
  std::shared_ptr<VirtualMachine> vm;
  uint32_t nextId = 0xff000000;
  std::shared_ptr<IPapyrusCompatibilityPolicy> policy;
  std::unordered_map<uint32_t, MpChangeForm> changeFormsForDeferredLoad;
  bool chunkLoadingInProgress = false;
  bool formLoadingInProgress = false;
  std::vector<RelootTimeForTypesEntry> relootTimeForTypes;
  std::set<std::string> forbiddenRelootTypes;
  std::vector<std::unique_ptr<IPapyrusClassBase>> classes;
  std::array<std::shared_ptr<std::vector<uint32_t>>, 0x100>
    allFormsByModIndexCache;
  std::vector<uint32_t> attachEspmRecordFailures;
  std::function<void(MpActor&)> beforeCampaignVisibility;
  std::function<void(MpActor&, bool)> afterCampaignVisibility;
  bool campaignVisibilityCommitting = false;
  std::optional<std::unordered_set<uint32_t>> campaignEnableLinks;
};

WorldState::WorldState()
  : worldStartTime(std::chrono::steady_clock::now())
{
  logger.reset(new spdlog::logger("empty logger"));

  pImpl.reset(new Impl);
  pImpl->policy = PapyrusCompatibilityPolicyFactory::Create(this);
}

WorldState::~WorldState()
{
  Clear();
}

void WorldState::Clear()
{
  if (pImpl->clearing)
    return;
  pImpl->clearing = true;
  for (const auto& batch : pImpl->sealedSaves) {
    batch->abandoned = true;
    if (batch->receipt && batch->receipt->state == SaveReceipt::State::Pending)
      batch->receipt->state = SaveReceipt::State::Abandoned;
  }
  // A submitted write may still commit. Retain it only as a draining barrier
  // if this WorldState is reused; never replay abandoned work or acknowledge
  // it as durable to its old caller. Unsubmitted work can be discarded.
  if (!pImpl->sealedSaves.empty() && pImpl->sealedSaves.front()->submitted) {
    auto keep = pImpl->sealedSaves.begin();
    pImpl->queuedReceiptBytes = (*keep)->receiptBytes;
    pImpl->sealedSaves.erase(++keep, pImpl->sealedSaves.end());
  } else {
    pImpl->sealedSaves.clear();
    pImpl->queuedReceiptBytes = 0;
  }
  forms.clear();
  grids.clear();
  formIdxManager.reset();
  refrByIdxUnreliable.clear();
  actorIdByProfileId.clear();
  actorIdByPrivateIndexedProperty.clear();
  hosters.clear();
  activationChildsByActivationParent.clear();
  lastMovUpdateByIdx.clear();
  pImpl->changeFormsForDeferredLoad.clear();
  pImpl->attachEspmRecordFailures.clear();
  for (auto& cache : pImpl->allFormsByModIndexCache)
    cache.reset();
  timerEffects.Clear();
  timerRegular.Clear();
  for (auto& entry : pImpl->relootTimeForTypes)
    entry.timer.Clear();
  pImpl->vm.reset();
  pImpl->classes.clear();
  pImpl->policy.reset();
  pImpl->changesByIdx.clear();
  pImpl->changesByIdxEmpty = true;
  pImpl->clearing = false;
}

const std::chrono::steady_clock::time_point& WorldState::GetStartPoint() const
{
  return worldStartTime;
}

void WorldState::AttachEspm(espm::Loader* espm_,
                            const FormCallbacksFactory& formCallbacksFactory_)
{
  pImpl->campaignEnableLinks.reset();
  espm = espm_;
  formCallbacksFactory = formCallbacksFactory_;
  espmCache.reset(new espm::CompressedFieldsCache);
  espmFiles = espm->GetFileNames();
}

void WorldState::AttachSaveStorage(
  std::shared_ptr<
    Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
    saveStorage)
{
  if (saveStorage == pImpl->saveStorage)
    return;
  if (QuestJournal::HasActiveTransaction() ||
      pImpl->campaignVisibilityCommitting ||
      (pImpl->saveStorage &&
       (!pImpl->sealedSaves.empty() || !pImpl->changesByIdxEmpty ||
        pImpl->saveTickActive)))
    throw std::runtime_error(
      "Cannot replace save storage with outstanding writes");
  spdlog::info("AttachSaveStorage - db fixes installed");

  pImpl->saveStorage = saveStorage;
}

void WorldState::AttachScriptStorage(
  std::shared_ptr<IScriptStorage> scriptStorage)
{
  pImpl->scriptStorage = scriptStorage;
}

void WorldState::AddForm(std::unique_ptr<MpForm> form, uint32_t formId,
                         bool skipChecks,
                         const MpChangeForm* optionalChangeFormToApply)
{
  if (!skipChecks && forms.find(formId) != forms.end()) {
    throw std::runtime_error(
      fmt::format("Form with id {:x} already exists", formId));
  }
  uint32_t campaignOriginal = 0;
  auto* reference = form->AsObjectReference();
  if (optionalChangeFormToApply &&
      optionalChangeFormToApply->dynamicFields.GetValueDump(
        CampaignReferenceBinding::kProperty) != "null") {
    campaignOriginal = CampaignReferenceBinding::SavedOriginal(
      *this, *optionalChangeFormToApply);
    if (!reference ||
        optionalChangeFormToApply->formDesc.ToFormId(espmFiles) != formId ||
        optionalChangeFormToApply->baseDesc.ToFormId(espmFiles) !=
          reference->GetBaseId() ||
        bool(reference->AsActor()) !=
          (optionalChangeFormToApply->recType == MpChangeForm::ACHR))
      throw std::runtime_error("Invalid saved campaign reference object");
  }

  // Assign formIndex before Init
  if (auto refr = form->AsObjectReference()) {
    if (!formIdxManager) {
      formIdxManager.reset(new MakeID(FormIndex::g_invalidIdx - 1));
    }

    if (!formIdxManager->CreateID(refr->idx)) {
      throw std::runtime_error("CreateID failed");
    }

    if (refrByIdxUnreliable.size() <= refr->GetIdx()) {
      refrByIdxUnreliable.resize(refr->GetIdx() + 1, nullptr);
    }
    refrByIdxUnreliable[refr->GetIdx()] = refr;
  }

  // MpObjectReference::Init requests save for newly created forms. That's why
  // we want formIndex to be assigned before init.
  if (campaignOriginal) {
    // Saved campaign copies already contain their selected template,
    // inventory and actor state. Ordinary Init/ApplyChangeForm would reroll
    // inventory, reset attributes and schedule corpse respawns.
    form->MpForm::Init(this, formId, true);
    reference->EditChangeForm(
      [&](MpChangeForm& state) { state = *optionalChangeFormToApply; },
      ChangeFormGuard::Mode::NoRequestSave);
  } else {
    form->Init(this, formId, optionalChangeFormToApply != nullptr);
  }

  auto it = forms.insert({ formId, std::move(form) }).first;

  if (optionalChangeFormToApply) {
    auto refr = it->second->AsObjectReference();
    if (!refr) {
      forms.erase(it); // Rollback changes due to exception
      throw std::runtime_error(
        "Unable to apply ChangeForm, cast to ObjectReference failed");
    }
    if (campaignOriginal) {
      refr->blockSaving = true;
      Viet::ScopedTask<MpObjectReference> unblock(
        [](MpObjectReference& value) { value.blockSaving = false; }, *refr);
      optionalChangeFormToApply->dynamicFields.ForEachValueDump(
        [&](const std::string& key, const std::string& value) {
          if (key.starts_with("private.indexed."))
            refr->RegisterPrivateIndexedProperty(key, value);
        });
      const auto source = GetEspm().GetBrowser().LookupById(campaignOriginal);
      const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                          ->GetData(GetEspmCache());
      if (data.boundsDiv2)
        refr->SetPrimitive(
          { data.boundsDiv2[0], data.boundsDiv2[1], data.boundsDiv2[2] });
      if (auto* actor = refr->AsActor())
        actor->factionsLoaded =
          optionalChangeFormToApply->factions.has_value();
      // Index spatially without firing load events while the owner and other
      // saved references may still be absent. A real subscriber loads scripts.
      if (!refr->IsDisabled() && !refr->IsDeleted()) {
        const auto cell = refr->GetCellOrWorld().ToFormId(espmFiles);
        refr->MoveOnGrid(*grids[cell].grid);
      }
    } else {
      refr->ApplyChangeForm(*optionalChangeFormToApply);
    }
  }
}

void WorldState::Tick()
{
  const auto now = std::chrono::system_clock::now();
  TickSaveStorage(now);
  TickTimers(now);
}

void WorldState::LoadChangeForm(const MpChangeForm& changeForm,
                                const FormCallbacks& callbacks)
{
  if (changeForm.dynamicFields.GetValueDump(
        CampaignReferenceBinding::kProperty) != "null")
    CampaignReferenceBinding::SavedOriginal(*this, changeForm);
  Viet::ScopedTask<bool> task([](bool& st) { st = false; },
                              pImpl->formLoadingInProgress);
  pImpl->formLoadingInProgress = true;

  std::unique_ptr<MpObjectReference> form;

  const auto baseId = changeForm.baseDesc.ToFormId(espmFiles);
  const auto formId = changeForm.formDesc.ToFormId(espmFiles);

  std::string baseType = "STAT";
  if (espm) {
    const auto rec = espm->GetBrowser().LookupById(baseId).rec;

    if (!rec) {
      spdlog::warn("Skipping ChangeForm {:x}: unable to find base record {:x}",
                   formId, baseId);
      return;
    }
    baseType = rec->GetType().ToString();
  }

  if (espm) {
    if (!changeForm.appearanceDump.empty()) {
      simdjson::dom::parser p;
      auto res = p.parse(changeForm.appearanceDump);
      if (res.error()) {
        spdlog::warn(
          "Skipping ChangeForm {:x}: unable to parse appearanceDump", formId);
        return;
      }

      Appearance appearance;
      try {
        appearance = Appearance::FromJson(res.value());
      } catch (std::exception& e) {
        spdlog::warn("Skipping ChangeForm {:x}: Appearance::FromJson failed "
                     "on appearanceDump: {}",
                     formId, e.what());
        return;
      }

      const auto rec = espm->GetBrowser().LookupById(appearance.raceId).rec;

      if (!rec || rec->GetType().ToString() != "RACE") {
        spdlog::warn("Skipping ChangeForm {:x}: bad raceId in appearanceDump",
                     formId);
        return;
      }
    }
  }

  if (formId < 0xff000000) {
    auto it = forms.find(formId);
    if (it != forms.end()) {
      auto refr = std::dynamic_pointer_cast<MpObjectReference>(it->second);
      if (refr) {
        refr->ApplyChangeForm(changeForm);
      }
    } else {
      pImpl->changeFormsForDeferredLoad[formId] = changeForm;
    }
    return;
  }

  switch (changeForm.recType) {
    case MpChangeForm::ACHR:
      form.reset(new MpActor(LocationalData(), callbacks, baseId));
      break;
    case MpChangeForm::REFR:
      form.reset(new MpObjectReference(LocationalData(), callbacks, baseId,
                                       baseType.data()));
      break;
    default:
      throw std::runtime_error("Unknown ChangeForm type: " +
                               std::to_string(changeForm.recType));
  }

  auto formRawPtr = reinterpret_cast<MpObjectReference*>(form.get());
  AddForm(std::move(form), formId, false, &changeForm);
  auto idx = formRawPtr->GetIdx();

  // EnsureBaseContainerAdded forces saving here.
  // We do not want characters to save when they are load partially
  // This behaviour results in
  // https://github.com/skyrim-multiplayer/issue-tracker/issues/64

  // So we expect that RequestSave does nothing in this case:

  if (pImpl->changesByIdx.size() > idx &&
      pImpl->changesByIdx[idx] != std::nullopt) {
    assert(false);

    // For Release configuration we just manually remove formId from changes
    pImpl->changesByIdx[idx] = std::nullopt;
  }
}

void WorldState::RequestReloot(MpObjectReference& ref,
                               std::chrono::system_clock::duration time)
{
  auto refPtr = &ref;

  std::optional<Viet::Promise<Viet::Void>> timer;

  constexpr auto kEpsilon = std::chrono::milliseconds(1);

  for (auto& entry : pImpl->relootTimeForTypes) {
    auto diff = entry.time - time;
    if (diff < kEpsilon && diff > -kEpsilon) {
      timer = entry.timer.SetTimer(time, nullptr);
      break;
    }
  }

  if (timer == std::nullopt) {
    timer = SetTimer(time);
  }

  timer->Then([this, refPtr, formId = ref.GetFormId()](Viet::Void) {
    auto& form = LookupFormById(formId);
    auto reference = form->AsObjectReference();
    // Check if the reference is still the same, not re-created with the same
    // id for example
    if (reference && reference == refPtr) {
      reference->DoReloot();
    }
  });
}

void WorldState::RequestSave(MpObjectReference& ref)
{
  if (pImpl->formLoadingInProgress || pImpl->clearing) {
    return;
  }

  auto idx = ref.GetIdx();

  [[unlikely]] if (idx == FormIndex::g_invalidIdx) {
    return spdlog::error("RequestSave {:x} - Invalid index", ref.GetFormId());
  }

  if (pImpl->changesByIdx.size() <= idx) {
    pImpl->changesByIdx.resize(idx + 1);
  }

  pImpl->changesByIdx[idx] = ref.GetChangeForm();
  pImpl->changesByIdxEmpty = false;
}

void WorldState::SetCampaignVisibilityCallbacks(
  std::function<void(MpActor&)> beforeCommit,
  std::function<void(MpActor&, bool)> afterCommit)
{
  if (pImpl->campaignVisibilityCommitting)
    throw std::runtime_error(
      "Cannot replace an active campaign visibility barrier");
  if (bool(beforeCommit) != bool(afterCommit))
    throw std::invalid_argument(
      "Both campaign visibility callbacks are required");
  pImpl->beforeCampaignVisibility = std::move(beforeCommit);
  pImpl->afterCampaignVisibility = std::move(afterCommit);
}

bool WorldState::HasCampaignEnableLinks(uint32_t original)
{
  if (!pImpl->campaignEnableLinks) {
    std::unordered_set<uint32_t> linked;
    const auto& browser = GetEspm().GetBrowser();
    // Browser's REFR collection contains both placed REFR and ACHR records;
    // ACHR is not a separate GetRecordsByType query. Distinct resolves winning
    // overrides for both kinds before their enable links are indexed.
    for (const auto& record : browser.GetDistinctRecordsByType("REFR")) {
      if (record.rec->IsDeleted())
        continue;
      bool seen = false;
      espm::RecordHeaderAccess::IterateFields(
        record.rec,
        [&](const char* tag, uint32_t size, const char* data) {
          if (std::memcmp(tag, "XESP", 4))
            return;
          if (seen || size != 8)
            throw std::runtime_error(
              "Invalid campaign enable-parent metadata");
          seen = true;
          uint32_t parent;
          std::memcpy(&parent, data, sizeof(parent));
          if (parent) {
            linked.insert(record.ToGlobalId(record.rec->GetId()));
            linked.insert(record.ToGlobalId(parent));
          }
        },
        GetEspmCache());
    }
    pImpl->campaignEnableLinks.emplace(std::move(linked));
  }
  return pImpl->campaignEnableLinks->contains(original);
}

void WorldState::ValidateCampaignFactions(const std::vector<Faction>& factions)
{
  if (factions.size() > Faction::kMaxEntries)
    throw std::runtime_error("Campaign faction limit exceeded");
  std::set<uint32_t> unique;
  for (const auto& faction : factions) {
    const auto id = faction.formDesc.ToFormId(espmFiles);
    const auto record = GetEspm().GetBrowser().LookupById(id);
    if (!id || !record.rec || record.rec->IsDeleted() ||
        record.rec->GetType() != "FACT" || !unique.insert(id).second ||
        FormDesc::FromFormId(id, espmFiles) != faction.formDesc)
      throw std::runtime_error("Invalid or duplicate campaign faction");
  }
}

void WorldState::RequireDurableSaveStorage() const
{
  if (pImpl->clearing || pImpl->formLoadingInProgress || !pImpl->saveStorage ||
      !pImpl->saveStorage->SupportsAtomicBatches() ||
      !pImpl->saveStorage->SupportsUpsertResults())
    throw std::runtime_error(
      "Durable campaign commit requires atomic save storage with results");
  if (pImpl->sealedSaves.size() >= Impl::maxSaveBatches ||
      pImpl->queuedReceiptBytes >= Impl::maxReceiptBytes ||
      pImpl->nextSaveSequence == std::numeric_limits<uint64_t>::max())
    throw std::runtime_error("Durable campaign save queue limit exceeded");
}

std::shared_ptr<const SaveReceipt> WorldState::CommitCampaignInventory(
  MpActor& owner, const std::string& journal,
  const std::vector<CampaignInventoryChange>& changes,
  const std::vector<CampaignVisibilityChange>& visibility,
  const std::vector<CampaignFactionChange>& factions,
  const std::function<void()>& validateReads, bool durable)
{
  if (durable)
    RequireDurableSaveStorage();
  if (pImpl->formLoadingInProgress)
    throw std::runtime_error("Campaign commit during world loading");
  if (pImpl->saveStorage && !pImpl->saveStorage->SupportsAtomicBatches())
    throw std::runtime_error(
      "Campaign inventory requires atomic save storage");
  struct Prepared
  {
    MpObjectReference* reference;
    std::shared_ptr<MpForm> lifetime;
    nlohmann::json initial;
    uint64_t epoch;
    MpChangeForm live;
    bool inventoryChanged = false;
    bool visibilityChanged = false;
    bool factionsChanged = false;
  };
  std::map<uint32_t, Prepared> prepared;
  auto prepare = [&](MpObjectReference& reference) -> Prepared& {
    const auto id = reference.GetFormId();
    if (reference.GetParent() != this ||
        LookupFormByIdNoLoad(id).get() != &reference ||
        reference.GetIdx() == FormIndex::g_invalidIdx)
      throw std::runtime_error("Unregistered campaign inventory participant");
    auto found = prepared.find(id);
    if (found != prepared.end())
      return found->second;
    const auto saved = reference.GetChangeForm();
    return prepared
      .emplace(id,
               Prepared{ &reference, LookupFormByIdNoLoad(id),
                         MpChangeForm::ToJson(saved),
                         reference.GetWorldInstanceEpoch(), saved })
      .first->second;
  };
  auto& ownerChange = prepare(owner);
  ownerChange.live.dynamicFields.SetValueDump(QuestJournal::kProperty,
                                              journal);
  std::set<uint32_t> unique;
  for (const auto& change : changes) {
    if (!change.reference ||
        !unique.insert(change.reference->GetFormId()).second)
      throw std::runtime_error(
        "Invalid or duplicate campaign inventory participant");
    auto& entry = prepare(*change.reference);
    entry.inventoryChanged = entry.live.inv != change.inventory;
    entry.live.inv = change.inventory;
    entry.live.baseContainerAdded = true;
  }
  std::set<uint32_t> visibilityIds;
  for (const auto& change : visibility) {
    if (!change.reference ||
        !visibilityIds.insert(change.reference->GetFormId()).second ||
        CampaignReferenceBinding::Context(*change.reference).first != &owner)
      throw std::runtime_error("Invalid campaign visibility participant");
    change.reference->ValidateCampaignVisibilityChange();
    auto& entry = prepare(*change.reference);
    entry.visibilityChanged = entry.live.isDisabled != change.disabled;
    entry.live.isDisabled = change.disabled;
  }
  std::set<uint32_t> factionIds;
  for (const auto& change : factions) {
    if (!change.actor ||
        !factionIds.insert(change.actor->GetFormId()).second ||
        (change.actor != &owner &&
         CampaignReferenceBinding::Context(*change.actor).first != &owner))
      throw std::runtime_error("Invalid campaign faction participant");
    ValidateCampaignFactions(change.factions);
    auto& entry = prepare(*change.actor);
    entry.factionsChanged = true;
    entry.live.factions = change.factions;
  }
  const bool changesVisibility =
    std::any_of(prepared.begin(), prepared.end(), [](const auto& item) {
      return item.second.visibilityChanged;
    });
  auto beforeVisibility = pImpl->beforeCampaignVisibility;
  auto afterVisibility = pImpl->afterCampaignVisibility;
  const bool requiresBarrier = changesVisibility || !factions.empty();
  if (requiresBarrier &&
      (!beforeVisibility || !afterVisibility ||
       pImpl->campaignVisibilityCommitting))
    throw std::runtime_error(
      "Campaign world state requires an available projection barrier");
  struct VisibilityGuard
  {
    bool& active;
    bool owns;
    MpActor& owner;
    const std::function<void(MpActor&, bool)>& finish;
    bool pendingCleanup = false;
    ~VisibilityGuard()
    {
      if (!owns)
        return;
      if (pendingCleanup) {
        try {
          finish(owner, false);
        } catch (...) {
        } // Preserve the failure which prevented publication.
      }
      active = false;
    }
  } guard{ pImpl->campaignVisibilityCommitting, requiresBarrier, owner,
           afterVisibility };
  if (requiresBarrier) {
    pImpl->campaignVisibilityCommitting = true;
    guard.pendingCleanup = true;
    beforeVisibility(owner);
  }
  validateReads();
  for (const auto& [id, entry] : prepared) {
    if (LookupFormByIdNoLoad(id) != entry.lifetime ||
        entry.reference->GetWorldInstanceEpoch() != entry.epoch ||
        MpChangeForm::ToJson(entry.reference->GetChangeForm()) !=
          entry.initial)
      throw std::runtime_error(
        "Campaign commit context changed at its projection barrier");
    if (entry.visibilityChanged)
      entry.reference->ValidateCampaignVisibilityChange();
  }

  // Clone grids and the current save buffer after the barrier callback, so
  // unrelated callback work is not overwritten by an older world snapshot.
  std::optional<decltype(grids)> nextGrids;
  std::optional<decltype(hosters)> nextHosters;
  if (changesVisibility) {
    nextGrids.emplace(grids);
    nextHosters.emplace(hosters);
  }
  std::set<uint32_t> clonedGrids;
  for (const auto& [id, entry] : prepared) {
    if (!entry.visibilityChanged)
      continue;
    const auto cell = entry.live.worldOrCellDesc.ToFormId(espmFiles);
    auto& grid = nextGrids->operator[](cell).grid;
    if (clonedGrids.insert(cell).second)
      grid = std::make_shared<GridImpl<MpObjectReference*>>(*grid);
    if (entry.live.isDisabled) {
      grid->Forget(entry.reference);
      for (auto lease = nextHosters->begin(); lease != nextHosters->end();) {
        if (lease->first == id || lease->second == id)
          lease = nextHosters->erase(lease);
        else
          ++lease;
      }
    } else
      entry.reference->MoveOnGrid(*grid);
  }
  size_t size = pImpl->changesByIdx.size();
  for (auto& [id, entry] : prepared) {
    // All serialization and allocation failures occur before publication.
    MpChangeForm::ToJson(entry.live).dump();
    size = std::max(size, static_cast<size_t>(entry.reference->GetIdx()) + 1);
  }
  // A whole change form is not nothrow-movable on every standard library
  // (MSVC's associative containers can allocate during a move). Prepare the
  // complete pending save buffer, then publish it through vector::swap.
  auto saveBatch = pImpl->changesByIdx;
  saveBatch.resize(size);
  for (const auto& [id, entry] : prepared)
    saveBatch[entry.reference->GetIdx()] = entry.live;
  std::shared_ptr<SaveReceipt> receipt;
  std::list<std::shared_ptr<Impl::SaveBatch>> sealed;
  std::vector<std::optional<MpChangeForm>> nextPending;
  size_t receiptBytes = 0;
  if (durable) {
    // The barrier may have performed unrelated work or replaced storage.
    RequireDurableSaveStorage();
    // Indexed save buffers can be very sparse. Charge their slots as well as
    // serialized values so many owner-only receipts cannot retain unbounded
    // empty change-form allocations.
    if (saveBatch.capacity() >
        Impl::maxReceiptBytes / sizeof(std::optional<MpChangeForm>))
      throw std::runtime_error("Durable campaign save batch is too large");
    receiptBytes = saveBatch.capacity() * sizeof(std::optional<MpChangeForm>);
    for (const auto& form : saveBatch) {
      if (!form)
        continue;
      const auto bytes = MpChangeForm::ToJson(*form).dump().size();
      if (bytes > Impl::maxReceiptBytes - receiptBytes)
        throw std::runtime_error("Durable campaign save batch is too large");
      receiptBytes += bytes;
    }
    if (receiptBytes > Impl::maxReceiptBytes - pImpl->queuedReceiptBytes)
      throw std::runtime_error(
        "Durable campaign save queue byte limit exceeded");
    receipt = std::make_shared<SaveReceipt>();
    receipt->sequence = pImpl->nextSaveSequence + 1;
    receipt->campaign = ownerChange.live.formDesc;
    receipt->journalRevision =
      nlohmann::json::parse(journal).at("revision").get<uint64_t>();
    auto batch = std::make_shared<Impl::SaveBatch>();
    batch->forms.swap(saveBatch);
    batch->receipt = receipt;
    batch->receiptBytes = receiptBytes;
    sealed.push_back(std::move(batch));
    nextPending.resize(size);
  }
  static_assert(noexcept(pImpl->changesByIdx.swap(saveBatch)));
  static_assert(noexcept(grids.swap(*nextGrids)));
  static_assert(noexcept(hosters.swap(*nextHosters)));
  for (auto& [id, entry] : prepared) {
    entry.reference->CommitPreparedCampaignChange(entry.live);
    if (entry.factionsChanged)
      entry.reference->AsActor()->factionsLoaded = true;
  }
  if (nextGrids) {
    grids.swap(*nextGrids);
    hosters.swap(*nextHosters);
  }
  if (durable) {
    pImpl->changesByIdx.swap(nextPending);
    pImpl->changesByIdxEmpty = true;
    pImpl->sealedSaves.splice(pImpl->sealedSaves.end(), sealed);
    pImpl->queuedReceiptBytes += receiptBytes;
    pImpl->nextSaveSequence = receipt->sequence;
  } else {
    pImpl->changesByIdx.swap(saveBatch);
    pImpl->changesByIdxEmpty = false;
  }
  // Publication has succeeded. A notification failure must not pretend the
  // committed quest was rolled back, or allow its rewards to be run twice.
  bool synchronized = true;
  for (const auto& [id, entry] : prepared) {
    if (!entry.visibilityChanged)
      continue;
    try {
      if (LookupFormByIdNoLoad(id) != entry.lifetime ||
          entry.reference->GetWorldInstanceEpoch() != entry.epoch)
        throw std::runtime_error(
          "Committed campaign reference is no longer available");
      entry.reference->ReconcileCampaignVisibility();
    } catch (...) {
      synchronized = false;
    }
  }
  for (const auto& [id, entry] : prepared) {
    if (!entry.inventoryChanged)
      continue;
    try {
      entry.reference->SendInventoryUpdate();
    } catch (...) {
      synchronized = false;
      try {
        spdlog::error(
          "Campaign inventory notification {:x} could not be queued", id);
      } catch (...) {
      } // State is already committed, even if logging fails.
    }
  }
  for (const auto& [id, entry] : prepared) {
    if (!entry.factionsChanged)
      continue;
    try {
      if (LookupFormByIdNoLoad(id) != entry.lifetime ||
          entry.reference->GetWorldInstanceEpoch() != entry.epoch)
        throw std::runtime_error(
          "Committed campaign actor is no longer available");
      entry.reference->AsActor()->SendCampaignFactionsUpdate();
    } catch (...) {
      synchronized = false;
      try {
        spdlog::error("Campaign faction notification {:x} could not be queued",
                      id);
      } catch (...) {
      }
    }
  }
  if (requiresBarrier) {
    if (LookupFormByIdNoLoad(owner.GetFormId()) != ownerChange.lifetime ||
        owner.GetWorldInstanceEpoch() != ownerChange.epoch)
      synchronized = false;
    guard.pendingCleanup = false;
    try {
      afterVisibility(owner, synchronized);
    } catch (...) {
      // The barrier remains closed; committed rewards must not be retried.
      try {
        spdlog::error("Campaign visibility completion could not be delivered");
      } catch (...) {
      }
    }
  }
  return receipt;
}

void WorldState::CommitCampaignReferences(
  MpActor& owner, MpChangeForm ownerState,
  const std::vector<MpChangeForm>& copies)
{
  if (pImpl->formLoadingInProgress || QuestJournal::HasActiveTransaction() ||
      owner.GetParent() != this ||
      LookupFormByIdNoLoad(owner.GetFormId()).get() != &owner ||
      !formIdxManager || owner.GetIdx() == FormIndex::g_invalidIdx)
    throw std::runtime_error("Invalid campaign allocation context");
  if (pImpl->saveStorage && !pImpl->saveStorage->SupportsAtomicBatches())
    throw std::runtime_error("Campaign allocation requires atomic storage");

  // Build replacement indexes off to the side. Existing objects retain their
  // addresses; only grids receiving new copies are cloned. No subscriptions,
  // Papyrus, timers or external callbacks run during preparation/publication.
  auto nextForms = forms;
  auto nextIndex = actorIdByPrivateIndexedProperty;
  auto nextGrids = grids;
  auto nextByIdx = refrByIdxUnreliable;
  auto saves = pImpl->changesByIdx;
  std::set<uint32_t> clonedGrids;
  std::vector<uint32_t> reserved;
  reserved.reserve(copies.size());
  try {
    for (const auto& saved : copies) {
      const auto original =
        CampaignReferenceBinding::SavedOriginal(*this, saved);
      const auto id = saved.formDesc.ToFormId(espmFiles);
      const auto prefix =
        "campaign:" + std::to_string(owner.GetFormId()) + ":";
      if (nextForms.contains(id) || !saved.worldInstance.starts_with(prefix))
        throw std::runtime_error("Campaign copy allocation conflict");
      const auto base = saved.baseDesc.ToFormId(espmFiles);
      const auto baseRecord = GetEspm().GetBrowser().LookupById(base);
      if (!baseRecord.rec || baseRecord.rec->IsDeleted())
        throw std::runtime_error("Missing campaign copy base");
      std::shared_ptr<MpObjectReference> copy;
      if (saved.recType == MpChangeForm::ACHR)
        copy = std::make_shared<MpActor>(LocationalData(),
                                         formCallbacksFactory(), base);
      else
        copy = std::make_shared<MpObjectReference>(
          LocationalData(), formCallbacksFactory(), base,
          baseRecord.rec->GetType().ToString().c_str());
      uint32_t idx;
      if (!formIdxManager->CreateID(idx))
        throw std::runtime_error("Campaign reference index exhausted");
      reserved.push_back(idx);
      copy->idx = idx;
      copy->MpForm::Init(this, id, true);
      copy->EditChangeForm([&](MpChangeForm& value) { value = saved; },
                           ChangeFormGuard::Mode::NoRequestSave);
      const auto source = GetEspm().GetBrowser().LookupById(original);
      const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                          ->GetData(GetEspmCache());
      if (data.boundsDiv2)
        copy->SetPrimitive(
          { data.boundsDiv2[0], data.boundsDiv2[1], data.boundsDiv2[2] });
      if (auto* actor = copy->AsActor())
        actor->factionsLoaded = saved.factions.has_value();
      saved.dynamicFields.ForEachValueDump(
        [&](const auto& key, const auto& value) {
          if (key.starts_with("private.indexed."))
            nextIndex[MakePrivateIndexedPropertyMapKey(key, value)].insert(id);
        });
      if (!saved.isDisabled && !saved.isDeleted) {
        const auto cell = saved.worldOrCellDesc.ToFormId(espmFiles);
        auto& grid = nextGrids[cell].grid;
        if (clonedGrids.insert(cell).second)
          grid = std::make_shared<GridImpl<MpObjectReference*>>(*grid);
        copy->MoveOnGrid(*grid);
      }
      nextByIdx.resize(std::max(nextByIdx.size(), size_t(idx) + 1), nullptr);
      nextByIdx[idx] = copy.get();
      MpChangeForm::ToJson(saved).dump();
      saves.resize(std::max(saves.size(), size_t(idx) + 1));
      saves[idx] = saved;
      nextForms.emplace(id, std::move(copy));
    }
    MpChangeForm::ToJson(ownerState).dump();
    saves.resize(std::max(saves.size(), size_t(owner.GetIdx()) + 1));
    saves[owner.GetIdx()] = ownerState;
  } catch (...) {
    for (auto it = reserved.rbegin(); it != reserved.rend(); ++it)
      formIdxManager->DestroyID(*it);
    throw;
  }
  static_assert(noexcept(forms.swap(nextForms)));
  static_assert(noexcept(actorIdByPrivateIndexedProperty.swap(nextIndex)));
  static_assert(noexcept(grids.swap(nextGrids)));
  forms.swap(nextForms);
  actorIdByPrivateIndexedProperty.swap(nextIndex);
  grids.swap(nextGrids);
  refrByIdxUnreliable.swap(nextByIdx);
  owner.CommitPreparedCampaignChange(ownerState);
  pImpl->changesByIdx.swap(saves);
  pImpl->changesByIdxEmpty = false;
  pImpl->allFormsByModIndexCache[0xff].reset();
}

const std::shared_ptr<MpForm>& WorldState::LookupFormById(
  uint32_t formId, std::stringstream* optionalOutTrace)
{
  static const std::shared_ptr<MpForm> kNullForm;

  if (optionalOutTrace) {
    *optionalOutTrace << "searching for " << std::hex << formId << std::endl;
  }

  auto it = forms.find(formId);
  if (it == forms.end()) {
    if (formId < 0xff000000) {
      if (LoadForm(formId, optionalOutTrace)) {
        it = forms.find(formId);
        if (it != forms.end()) {
          if (optionalOutTrace) {
            *optionalOutTrace << "found after successful LoadForm" << std::hex
                              << formId << std::endl;
          }
          return it->second;
        }
        if (optionalOutTrace) {
          *optionalOutTrace << "not found after successful LoadForm"
                            << std::hex << formId << std::endl;
        }
        return kNullForm;
      } else {
        if (optionalOutTrace) {
          *optionalOutTrace << "LoadForm returned false " << std::hex << formId
                            << std::endl;
        }
      }
    }
    if (optionalOutTrace) {
      *optionalOutTrace << "not found " << std::hex << formId << std::endl;
    }
    return kNullForm;
  }

  if (optionalOutTrace) {
    *optionalOutTrace << "found " << std::hex << formId << std::endl;
  }
  return it->second;
}

const std::shared_ptr<MpForm>& WorldState::LookupFormByIdNoLoad(
  uint32_t formId)
{
  static const std::shared_ptr<MpForm> kNullForm;

  auto it = forms.find(formId);
  if (it == forms.end()) {
    return kNullForm;
  }

  return it->second;
}

bool WorldState::AttachEspmRecord(const espm::CombineBrowser& br,
                                  const espm::RecordHeader* record,
                                  const espm::IdMapping& mapping,
                                  std::stringstream* optionalOutTrace)
{
  auto& cache = GetEspmCache();
  // this place is a hotpath.
  // we want to use reinterpret_cast<> instead of espm::Convert<>
  // in order to reduce amount of generated assembly code
  auto* refr = reinterpret_cast<const espm::REFR*>(record);
  auto data = refr->GetData(cache);

  uint32_t baseId = espm::utils::GetMappedId(data.baseId, mapping);
  espm::LookupResult base = br.LookupById(baseId);
  if (!base.rec) {
    logger->info("baseId {} {}", baseId, static_cast<const void*>(base.rec));
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format(
        "AttachEspmRecord - base record not found {:x} \n", baseId);
    }
    return false;
  }

  espm::Type t = base.rec->GetType();
  bool isNpc = t == "NPC_";
  bool isFurniture = t == "FURN";
  bool isActivator = t == "ACTI";
  bool isDoor = t == "DOOR";
  bool isContainer = t == "CONT";
  bool isFlor = t == "FLOR" &&
    reinterpret_cast<const espm::FLOR*>(base.rec)->GetData(cache).resultItem;
  bool isTree = t == "TREE" &&
    reinterpret_cast<const espm::TREE*>(base.rec)->GetData(cache).resultItem;

  if (!isNpc && !isFurniture && !isActivator && !espm::utils::IsItem(t) &&
      !isDoor && !isContainer && !isFlor && !isTree) {
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format(
        "AttachEspmRecord - the server skips base type {} \n", t.ToString());
    }
    return false;
  }

  bool startsDead = false;
  if (isNpc) {
    auto* achr = reinterpret_cast<const espm::ACHR*>(record);
    startsDead = achr->StartsDead();
    if (startsDead) {
      if (optionalOutTrace) {
        *optionalOutTrace << fmt::format(
          "AttachEspmRecord - the server skips dead actors\n");
      }
      return false; // TODO: Load dead references
    }
  }

  // TODO: Load disabled references
  enum
  {
    InitiallyDisabled = 0x800,
    DeletedRecord = 0x20
  };

  if (refr->GetFlags() & InitiallyDisabled) {
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format(
        "AttachEspmRecord - the server skips initially disabled references\n");
    }
    return false;
  }

  if (refr->GetFlags() & DeletedRecord) {
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format(
        "AttachEspmRecord - the server skips deleted references\n");
    }
    return false;
  }

  if (!npcEnabled && isNpc) {
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format(
        "AttachEspmRecord - the server skips npcs: npcEnabled = false\n");
    }
    return false;
  }

  uint32_t formId = espm::utils::GetMappedId(record->GetId(), mapping);

  if (isNpc) {
    if (NpcSourceFilesOverriden() && !IsNpcAllowed(formId)) {
      spdlog::trace("Skip NPC loading, it is not allowed. refrId {:#x}",
                    formId);
      if (optionalOutTrace) {
        *optionalOutTrace
          << fmt::format("Skip NPC loading, it is not allowed. refrId {:#x}",
                         formId)
          << std::endl;
      }
      return false;
    }
    auto npcData =
      reinterpret_cast<const espm::NPC_*>(base.rec)->GetData(cache);

    if (npcData.isEssential || npcData.isProtected || npcData.isUnique) {
      if (optionalOutTrace) {
        *optionalOutTrace << fmt::format("Skip NPC due to its flags")
                          << std::endl;
      }
      return false;
    }

    enum class ListType : uint32_t
    {
      CrimeFactionsList = 0x26953
    };

    auto factionBaseId = static_cast<std::underlying_type_t<ListType>>(
      ListType::CrimeFactionsList);
    espm::LookupResult res = br.LookupById(factionBaseId);
    auto* formList = reinterpret_cast<const espm::FLST*>(res.rec);
    std::vector<uint32_t> factionFormIds = formList->GetData(cache).formIds;
    for (auto& formId : factionFormIds) {
      formId = res.ToGlobalId(formId);
    }

    for (auto fact : npcData.factions) {
      auto it = std::find(factionFormIds.begin(), factionFormIds.end(),
                          base.ToGlobalId(fact.formId));
      if (it != factionFormIds.end()) {
        logger->info("Skipping actor {:#x} because it's in faction {:#x}",
                     record->GetId(), *it);
        if (optionalOutTrace) {
          *optionalOutTrace << fmt::format("Skip NPC due to faction")
                            << std::endl;
        }
        return false;
      }
    }

    const int kPcLevel = 0; // Shouldn't mean much to races

    // May be not exact the same template chain as it would be in MpActor
    // but it's ok for this check
    std::vector<uint32_t> evaluateChainResult =
      LeveledListUtils::EvaluateTemplateChain(br, base, kPcLevel);
    std::vector<FormDesc> templateChain(evaluateChainResult.size());
    std::transform(evaluateChainResult.begin(), evaluateChainResult.end(),
                   templateChain.begin(), [&](uint32_t formId) {
                     return FormDesc::FromFormId(formId, this->espmFiles);
                   });

    uint32_t race = EvaluateTemplate<espm::NPC_::UseTraits>(
      this, baseId, templateChain,
      [&](const auto& npcLookupResult, const auto& npcData) {
        return npcLookupResult.ToGlobalId(npcData.race);
      });

    bool isBanned = std::find(bannedEspmCharacterRaceIds.begin(),
                              bannedEspmCharacterRaceIds.end(),
                              race) != bannedEspmCharacterRaceIds.end();
    if (isBanned) {
      logger->info("Skipping actor {:#x} because it has banned race {:#x}",
                   record->GetId(), race);
      if (optionalOutTrace) {
        *optionalOutTrace << fmt::format("Skip NPC due to banned race")
                          << std::endl;
      }
      return false;
    }
  }

  auto locationalData = data.loc;

  uint32_t worldOrCell =
    espm::utils::GetMappedId(GetWorldOrCell(br, record), mapping);
  if (!worldOrCell) {
    logger->error("Anomaly: refr without world/cell");
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format("Anomaly: refr without world/cell")
                        << std::endl;
    }
    return false;
  }

  if (isNpc && NpcSourceFilesOverriden()) {
    bool isInterior = false, isExterior = false,
         spawnInInterior = defaultSetting.spawnInInterior,
         spawnInExterior = defaultSetting.spawnInExterior;
    espm::LookupResult res = br.LookupById(worldOrCell);
    auto* cellRecord = espm::Convert<espm::CELL>(res.rec);
    if (cellRecord) {
      isInterior = true;
    }
    auto* worldRecord = espm::Convert<espm::WRLD>(res.rec);
    if (worldRecord) {
      isExterior = true;
    }
    uint32_t npcFileIdx = GetFileIdx(formId);
    if (npcFileIdx >= espmFiles.size()) {
      spdlog::error("NPC's idx is greater than espmFiles.size(). NPC's"
                    "formId "
                    "{:#x}, espmFiles size: {}",
                    formId, espmFiles.size());
      if (optionalOutTrace) {
        *optionalOutTrace
          << fmt::format("NPC's idx is greater than espmFiles.size(). NPC's"
                         "formId "
                         "{:#x}, espmFiles size: {}",
                         formId, espmFiles.size())
          << std::endl;
      }
      return false;
    }
    auto it = npcSettings.find(espmFiles[npcFileIdx]);
    if (it != npcSettings.end()) {
      spawnInInterior = it->second.spawnInInterior;
      spawnInExterior = it->second.spawnInExterior;
      spdlog::trace(
        "found npc setting spawnInInterior: {}, spawnInExterior: {}",
        spawnInInterior, spawnInExterior);
    } else {
      spdlog::trace("npc setting has not been found. Use default "
                    "spawnInInterior: {} and spawnInExterior: {}",
                    spawnInInterior, spawnInExterior);
    }

    if (spawnInInterior && isInterior || spawnInExterior && isExterior) {
    }

    if ((!spawnInInterior || !isInterior) &&
        (!spawnInExterior || !isExterior)) {
      spdlog::trace(
        "Unable to spawn npc because of "
        "rules applied in server settings: spawnInInterior={}, "
        "spawnInExterior={}, NPC location: exterior={}, interior={}",
        spawnInInterior, spawnInExterior, isExterior, isInterior);
      if (optionalOutTrace) {
        *optionalOutTrace
          << fmt::format(
               "Unable to spawn npc because of "
               "rules applied in server settings: spawnInInterior={}, "
               "spawnInExterior={}, NPC location: exterior={}, interior={}",
               spawnInInterior, spawnInExterior, isExterior, isInterior)
          << std::endl;
      }
      return false;
    }
  }

  if (!locationalData) {
    logger->error("Anomaly: refr without locationalData");
    if (optionalOutTrace) {
      *optionalOutTrace << fmt::format("Anomaly: refr without locationalData")
                        << std::endl;
    }
    return false;
  }

  std::optional<NiPoint3> primitiveBoundsDiv2;
  if (data.boundsDiv2) {
    primitiveBoundsDiv2 =
      NiPoint3(data.boundsDiv2[0], data.boundsDiv2[1], data.boundsDiv2[2]);
  }

  auto typeStr = t.ToString();
  std::unique_ptr<MpForm> form;
  LocationalData formLocationalData = {
    LocationalDataUtils::GetPos(locationalData),
    LocationalDataUtils::GetRot(locationalData),
    FormDesc::FromFormId(worldOrCell, espmFiles)
  };

  MpChangeFormREFR* changeForm = nullptr;
  if (!isNpc) {
    form.reset(new MpObjectReference(formLocationalData,
                                     formCallbacksFactory(), baseId,
                                     typeStr.data(), primitiveBoundsDiv2));
  } else {
    form.reset(
      new MpActor(formLocationalData, formCallbacksFactory(), baseId));
  }
  AddForm(std::move(form), formId, true, changeForm);

  // Do not TriggerFormInitEvent here, doing it later after changeForm apply

  if (optionalOutTrace) {
    *optionalOutTrace << fmt::format("AttachEspmRecord returned true")
                      << std::endl;
  }
  return true;
}

bool WorldState::LoadForm(uint32_t formId, std::stringstream* optionalOutTrace)
{
  ANTIGO_CONTEXT_INIT(ctx);
  ctx.AddUnsigned(formId);

  auto& br = GetEspm().GetBrowser();

  auto lookupRes = br.LookupById(formId);

  if (!lookupRes.rec) {
    return false;
  }

  auto mapping = br.GetCombMapping(lookupRes.fileIdx);

  bool attached =
    AttachEspmRecord(br, lookupRes.rec, *mapping, optionalOutTrace);
  if (optionalOutTrace) {
    *optionalOutTrace << "AttachEspmRecord " << (attached ? "true" : "false")
                      << std::endl;
  }

  if (!attached) {
    pImpl->attachEspmRecordFailures.push_back(formId);
  }

  if (attached) {
    auto& refr = GetFormAt<MpObjectReference>(formId);
    auto it = pImpl->changeFormsForDeferredLoad.find(formId);
    if (it != pImpl->changeFormsForDeferredLoad.end()) {
      auto copy = it->second; // crashes without copying
      refr.ApplyChangeForm(copy);
    }

    refr.ForceSubscriptionsUpdate();
  }

  return attached;
}

void WorldState::TickSaveStorage(const std::chrono::system_clock::time_point&)
{
  if (!pImpl->saveStorage || pImpl->clearing || pImpl->saveTickActive)
    return;
  pImpl->saveTickActive = true;
  Viet::ScopedTask<bool> tickGuard([](bool& active) { active = false; },
                                   pImpl->saveTickActive);
  const auto storage = pImpl->saveStorage;
  try {
    storage->Tick();
  } catch (
    const Viet::AsyncSaveStorage<
      MpChangeForm, FormDesc, std::vector<FormDesc>>::UpsertFailedException&) {
    // Compatibility with old custom storages. Durable receipts require the
    // correlated result API, so an anonymous error can never fail their batch.
    if (!pImpl->sealedSaves.empty()) {
      auto& batch = *pImpl->sealedSaves.front();
      if (batch.submitted && !batch.tracked && !batch.resultReady) {
        batch.result = std::current_exception();
        batch.resultReady = true;
      }
    }
  } catch (const std::exception& error) {
    // Iterate/callback/recycling failures are not evidence about the current
    // write. In particular they cannot release its ordering barrier.
    spdlog::error("TickSaveStorage - unrelated storage error: {}",
                  error.what());
  } catch (...) {
    spdlog::error("TickSaveStorage - unrelated unknown storage error");
  }

  auto finish = [&] {
    if (pImpl->sealedSaves.empty())
      return;
    auto batch = pImpl->sealedSaves.front();
    if (!batch->resultReady)
      return;
    batch->submitted = false;
    batch->resultReady = false;
    if (batch->result && !batch->abandoned) {
      if (batch->receipt)
        batch->receipt->lastError = batch->result;
      // Retain the exact old snapshot, including unloaded/destroyed objects.
      // New live edits remain behind it and never participate in this retry.
      try {
        std::rethrow_exception(batch->result);
      } catch (const std::exception& error) {
        spdlog::error("TickSaveStorage - retained batch failed: {}",
                      error.what());
      } catch (...) {
        spdlog::error(
          "TickSaveStorage - retained batch failed with unknown error");
      }
      batch->result = {};
      return;
    }
    if (batch->receipt && !batch->abandoned) {
      batch->receipt->state = SaveReceipt::State::Durable;
      batch->receipt->lastError = {};
    }
    pImpl->queuedReceiptBytes -= batch->receiptBytes;
    pImpl->sealedSaves.pop_front();
  };
  finish();

  // Async storage recycles one indexed vector per successful write. Consume
  // those buffers even though we retain our own immutable retry snapshots;
  // otherwise its recycle queue would grow on every ordinary save.
  try {
    std::vector<std::optional<MpChangeForm>> recycled;
    for (size_t i = 0;
         i < 64 && storage->GetRecycledChangeFormsBuffer(recycled); ++i) {
      recycled.clear();
      if (pImpl->changesByIdxEmpty &&
          recycled.capacity() > pImpl->changesByIdx.capacity()) {
        const auto size = pImpl->changesByIdx.size();
        pImpl->changesByIdx.swap(recycled);
        pImpl->changesByIdx.resize(size);
      }
    }
  } catch (const std::exception& error) {
    spdlog::error("TickSaveStorage - optional buffer recycling failed: {}",
                  error.what());
  } catch (...) {
    spdlog::error("TickSaveStorage - optional buffer recycling failed");
  }

  // Coalesce ordinary updates only until submission. Existing sealed batches
  // always drain first; campaign allocation and ordinary journal updates use
  // this same pending buffer and cannot overtake an earlier receipt.
  if (pImpl->sealedSaves.empty() && !pImpl->changesByIdxEmpty) {
    try {
      auto batch = std::make_shared<Impl::SaveBatch>();
      std::vector<std::optional<MpChangeForm>> nextPending(
        pImpl->changesByIdx.size());
      pImpl->sealedSaves.push_back(batch);
      batch->forms.swap(pImpl->changesByIdx);
      pImpl->changesByIdx.swap(nextPending);
      pImpl->changesByIdxEmpty = true;
    } catch (const std::exception& error) {
      spdlog::error("TickSaveStorage - could not prepare save batch: {}",
                    error.what());
      return;
    }
  }
  if (pImpl->sealedSaves.empty())
    return;
  const auto batch = pImpl->sealedSaves.front();
  if (batch->submitted)
    return;
  try {
    if (batch->receipt &&
        (!storage->SupportsAtomicBatches() ||
         !storage->SupportsUpsertResults()))
      throw std::runtime_error(
        "Queued durable save requires atomic storage with results");
    // The storage interface takes ownership of its argument even on a throw.
    // Keep our own immutable copy until a matching success is observed.
    auto payload = batch->forms;
    if (batch->attempt == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Save batch attempt counter exhausted");
    const auto attempt = ++batch->attempt;
    batch->submitted = true;
    batch->tracked = storage->SupportsUpsertResults();
    if (batch->receipt)
      batch->receipt->attempts = attempt;
    std::weak_ptr<Impl> weak = pImpl;
    std::weak_ptr<Impl::SaveBatch> weakBatch = batch;
    const auto complete = [weak, weakBatch,
                           attempt](std::exception_ptr error) {
      const auto state = weak.lock();
      const auto pending = weakBatch.lock();
      if (!state || !pending || state->sealedSaves.empty() ||
          state->sealedSaves.front() != pending || !pending->submitted ||
          pending->attempt != attempt || pending->resultReady)
        return;
      pending->result = error;
      pending->resultReady = true;
    };
    if (batch->tracked)
      storage->UpsertWithResult(std::move(payload), complete);
    else
      storage->Upsert(std::move(payload), [complete] { complete({}); });
  } catch (...) {
    // An inline result is authoritative even if a buggy backend throws after
    // delivering it. Otherwise a synchronous submission failure is retryable.
    if (!batch->resultReady) {
      batch->result = std::current_exception();
      batch->resultReady = true;
    }
  }
  finish();
}

void WorldState::TickTimers(const std::chrono::system_clock::time_point&)
{
  timerEffects.TickTimers();
  timerRegular.TickTimers();

  for (auto& entry : pImpl->relootTimeForTypes) {
    entry.timer.TickTimers();
  }
}

void WorldState::SendPapyrusEvent(MpForm* form, const char* eventName,
                                  const VarValue* arguments,
                                  size_t argumentsCount)
{
  if (const auto* reference = form ? form->AsObjectReference() : nullptr;
      reference &&
      reference->GetDynamicFields().GetValueDump(
        CampaignReferenceBinding::kProperty) != "null")
    throw std::runtime_error(
      "Private references require the campaign event dispatcher");
  std::vector<VarValue> args = { arguments, arguments + argumentsCount };

  if (spdlog::should_log(spdlog::level::trace)) {
    std::vector<std::string> argsStrings(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      argsStrings[i] = args[i].ToString();
    }

    if (!strcmp(eventName, "OnTrigger")) {
      static std::once_flag g_once;
      std::call_once(g_once, [&] {
        spdlog::trace("WorldState::SendPapyrusEvent {:x} - {} [{}]",
                      form->GetFormId(), eventName,
                      fmt::join(argsStrings, ", "));
        spdlog::trace("WorldState::SendPapyrusEvent {:x} - Muting {} globally "
                      "to keep logs clear",
                      form->GetFormId(), eventName);
      });
    } else {
      spdlog::trace("WorldState::SendPapyrusEvent {:x} - {} [{}]",
                    form->GetFormId(), eventName,
                    fmt::join(argsStrings, ", "));
    }
  }

  VirtualMachine::OnEnter onEnter = [&](const StackData& stackData) {
    pImpl->policy->BeforeSendPapyrusEvent(
      form, eventName, arguments, argumentsCount,
      stackData.stackIdHolder.GetStackId());
  };
  auto& vm = GetPapyrusVm();
  return vm.SendEvent(form->ToGameObject(), eventName, args, onEnter);
}

const std::set<MpObjectReference*>& WorldState::GetNeighborsByPosition(
  uint32_t cellOrWorld, int16_t cellX, int16_t cellY)
{
  if (espm && !pImpl->chunkLoadingInProgress) {
    Viet::ScopedTask<bool> task([](bool& st) { st = false; },
                                pImpl->chunkLoadingInProgress);
    pImpl->chunkLoadingInProgress = true;

    auto& br = espm->GetBrowser();
    for (int16_t x = cellX - 1; x <= cellX + 1; ++x) {
      for (int16_t y = cellY - 1; y <= cellY + 1; ++y) {
        const bool loaded = grids[cellOrWorld].loadedChunks[x][y];
        if (!loaded) {
          for (size_t i = 0; i < espmFiles.size(); ++i) {
            auto combMapping = br.GetCombMapping(i);
            auto rawMapping = br.GetRawMapping(i);
            uint32_t mappedCellOrWorld =
              espm::utils::GetMappedId(cellOrWorld, *rawMapping);
            auto records = br.GetRecordsAtPos(mappedCellOrWorld, x, y);
            for (auto rec : *records[i]) {
              auto mappedId =
                espm::utils::GetMappedId(rec->GetId(), *combMapping);
              assert(mappedId < 0xff000000);
              LoadForm(mappedId);
            }
          }
          // Do not keep "loaded" reference here since LoadForm would
          // invalidate this reference
          grids[cellOrWorld].loadedChunks[x][y] = true;
        }
      }
    }
  }

  auto& neighbours =
    grids[cellOrWorld].grid->GetNeighboursByPosition(cellX, cellY);
  return neighbours;
}

std::shared_ptr<std::vector<uint32_t>> WorldState::GetAllForms(
  uint32_t modIndex)
{
  if (modIndex >= std::size(pImpl->allFormsByModIndexCache)) {
    spdlog::error("WorldState::GetAllForms - Invalid mod index {}", modIndex);
    return nullptr;
  }

  auto& resCache = pImpl->allFormsByModIndexCache[modIndex];

  if (!resCache) {
    // Deduplicate form IDs
    // TODO: Consider cleaning changeFormsForDeferredLoad after adding a form
    // so we don't need de-duplicate in runtime
    std::unordered_set<uint32_t> formIds;
    for (const auto& p : forms) {
      if ((p.first >> 24) == modIndex) {
        formIds.insert(p.first);
      }
    }
    for (const auto& p : pImpl->changeFormsForDeferredLoad) {
      if ((p.first >> 24) == modIndex) {
        formIds.insert(p.first);
      }
    }

    for (auto failedToAttachFormId : pImpl->attachEspmRecordFailures) {
      formIds.erase(failedToAttachFormId);
    }

    resCache =
      std::make_shared<std::vector<uint32_t>>(formIds.begin(), formIds.end());
  }

  return resCache;
}

MpForm* WorldState::LookupFormByIdx(int idx)
{
  if (formIdxManager) {
    if (idx >= 0 && idx < refrByIdxUnreliable.size()) {
      auto refr = refrByIdxUnreliable[idx];
      if (refr && refr->GetIdx() == idx) {
        return refr;
      }
    }
  }
  return nullptr;
}

espm::Loader& WorldState::GetEspm() const
{
  if (!espm) {
    throw std::runtime_error("No espm attached");
  }
  return *espm;
}

bool WorldState::HasEspm() const
{
  return !!espm;
}

espm::CompressedFieldsCache& WorldState::GetEspmCache()
{
  if (!espmCache) {
    throw std::runtime_error("No espm cache found");
  }
  return *espmCache;
}

IScriptStorage* WorldState::GetScriptStorage() const
{
  return pImpl->scriptStorage.get();
}

struct LazyState
{
  std::shared_ptr<PexScript> pex;
  std::vector<uint8_t> pexBin;

  // With Papyrus hotreload enabled, this variable hold references to
  // previous versions of pex files. This prevents the invalidation of
  // string/identifier types of VarValue
  std::vector<std::shared_ptr<PexScript>> oldPexHolder;
};

PexScript::Lazy CreatePexScriptLazy(
  const CIString& required, std::shared_ptr<IScriptStorage> scriptStorage,
  std::shared_ptr<spdlog::logger> logger, bool enableHotReload)
{
  auto lazyState = std::make_shared<LazyState>();

  PexScript::Lazy lazy;
  lazy.source = required.data();
  lazy.fn = [lazyState, scriptStorage, required, logger, enableHotReload]() {
    if (enableHotReload) {
      auto requiredPex = scriptStorage->GetScriptPex(required.data());
      if (requiredPex != lazyState->pexBin) {
        lazyState->oldPexHolder.push_back(lazyState->pex);
        lazyState->pex.reset();
        lazyState->pexBin = requiredPex;
        logger->info("Papyrus script {} has been reloaded", required);
      }
    }

    if (!lazyState->pex) {
      auto requiredPex = scriptStorage->GetScriptPex(required.data());
      if (requiredPex.empty()) {
        throw std::runtime_error(
          "'" + std::string({ required.begin(), required.end() }) +
          "' is listed but failed to "
          "load from the storage");
      }
      auto pexStructure = Reader({ requiredPex }).GetSourceStructures();
      lazyState->pex = pexStructure[0];
    }
    return lazyState->pex;
  };

  return lazy;
}

VirtualMachine& WorldState::GetPapyrusVm()
{
  if (!pImpl->policy)
    pImpl->policy = PapyrusCompatibilityPolicyFactory::Create(this);
  if (!pImpl->vm) {
    std::vector<PexScript::Lazy> pexStructures;
    std::vector<std::string> scriptNames;

    auto scriptStorage = pImpl->scriptStorage;
    if (!scriptStorage) {
      logger->error("Required scriptStorage to be non-null");
      pImpl->vm.reset(
        new VirtualMachine(std::vector<std::shared_ptr<PexScript>>()));
      return *pImpl->vm;
    }

    auto& scripts = scriptStorage->ListScripts(false);
    for (auto& required : scripts) {
      auto lazy = CreatePexScriptLazy(required, scriptStorage, this->logger,
                                      this->isPapyrusHotReloadEnabled);
      pexStructures.push_back(lazy);
    }

    if (!pexStructures.empty()) {
      pImpl->vm.reset(new VirtualMachine(pexStructures));

      pImpl->vm->SetMissingScriptHandler(
        [scriptStorage, this](std::string className) {
          std::optional<PexScript::Lazy> result;

          CIString classNameCi = { className.begin(), className.end() };
          if (scriptStorage->ListScripts(true).count(classNameCi)) {
            result =
              CreatePexScriptLazy(classNameCi, scriptStorage, this->logger,
                                  this->isPapyrusHotReloadEnabled);
          }
          return result;
        });

      pImpl->classes =
        PapyrusClassesFactory::CreateAndRegister(*pImpl->vm, pImpl->policy);
    }
  }

  return *pImpl->vm;
}

const std::set<uint32_t>& WorldState::GetActorsByProfileId(
  int32_t profileId) const
{
  static const std::set<uint32_t> kEmptySet;

  auto it = actorIdByProfileId.find(profileId);
  if (it == actorIdByProfileId.end()) {
    return kEmptySet;
  }
  return it->second;
}

const std::set<uint32_t>& WorldState::GetActorsByPrivateIndexedProperty(
  const std::string& privateIndexedPropertyMapKey) const
{
  static const std::set<uint32_t> kEmptySet;

  auto it = actorIdByPrivateIndexedProperty.find(privateIndexedPropertyMapKey);
  if (it == actorIdByPrivateIndexedProperty.end()) {
    return kEmptySet;
  }
  return it->second;
}

std::string WorldState::MakePrivateIndexedPropertyMapKey(
  const std::string& propertyName, const std::string& propertyValueStringified)
{
  return propertyName + '=' + propertyValueStringified;
}

uint32_t WorldState::GenerateFormId()
{
  while (pImpl->nextId >= 0xff000000) {
    const auto id = pImpl->nextId++;
    if (!LookupFormByIdNoLoad(id))
      return id;
  }
  throw std::overflow_error("Dynamic reference ID space exhausted");
}

void WorldState::SetRelootTime(const std::string& recordType,
                               std::chrono::system_clock::duration time)
{
  auto it = std::find_if(pImpl->relootTimeForTypes.begin(),
                         pImpl->relootTimeForTypes.end(),
                         [&recordType](const RelootTimeForTypesEntry& entry) {
                           return entry.recordType == recordType;
                         });

  if (it != pImpl->relootTimeForTypes.end()) {
    it->time = time;
  } else {
    pImpl->relootTimeForTypes.push_back({ recordType, time });
  }
}

std::optional<std::chrono::system_clock::duration> WorldState::GetRelootTime(
  const std::string& recordType) const
{
  auto it = std::find_if(pImpl->relootTimeForTypes.begin(),
                         pImpl->relootTimeForTypes.end(),
                         [&recordType](const RelootTimeForTypesEntry& entry) {
                           return entry.recordType == recordType;
                         });

  if (it == pImpl->relootTimeForTypes.end()) {
    return std::nullopt;
  }
  return it->time;
}

bool WorldState::HasKeyword(uint32_t baseId, const char* keyword)
{
  auto lookupRes = GetEspm().GetBrowser().LookupById(baseId);

  if (!lookupRes.rec) {
    return false;
  }

  const auto keywordIds = lookupRes.rec->GetKeywordIds(GetEspmCache());

  for (auto keywordId : keywordIds) {
    auto keywordIdGlobal = lookupRes.ToGlobalId(keywordId);
    auto rec = GetEspm().GetBrowser().LookupById(keywordIdGlobal).rec;
    if (rec && !Utils::stricmp(rec->GetEditorId(GetEspmCache()), keyword)) {
      return true;
    }
  }

  return false;
}

bool WorldState::NpcSourceFilesOverriden() const noexcept
{
  return !npcSettings.empty() || defaultSetting.overriden;
}

bool WorldState::IsNpcAllowed(uint32_t refrId) const noexcept
{
  if (npcSettings.empty() && defaultSetting.overriden) {
    return true;
  }
  uint32_t npcFileIdx = GetFileIdx(refrId);
  for (const auto& [fileName, _] : npcSettings) {
    auto it = std::find(espmFiles.begin(), espmFiles.end(), fileName);
    if (it == espmFiles.end()) {
      return false;
    }
    auto idx = std::distance(espmFiles.begin(), it);
    if (npcFileIdx == idx) {
      return true;
    }
  }
  return false;
}

uint32_t WorldState::GetFileIdx(uint32_t formId) const noexcept
{
  return formId >> 24;
}

void WorldState::SetNpcSettings(
  std::unordered_map<std::string, NpcSettingsEntry>&& settings)
{
  npcSettings = settings;
}

bool WorldState::RemoveTimer(uint32_t timerId)
{
  return timerRegular.RemoveTimer(timerId);
}

bool WorldState::RemoveEffectTimer(uint32_t timerId)
{
  return timerEffects.RemoveTimer(timerId);
}

void WorldState::SetForbiddenRelootTypes(const std::set<std::string>& types)
{
  pImpl->forbiddenRelootTypes = types;
}

void WorldState::SetEnableConsoleCommandsForAllSetting(bool enable)
{
  enableConsoleCommandsForAll = enable;
}

bool WorldState::IsRelootForbidden(std::string type) const noexcept
{
  return pImpl->forbiddenRelootTypes.find(type) !=
    pImpl->forbiddenRelootTypes.end();
}

bool WorldState::HasEspmFile(std::string_view filename) const noexcept
{
  return std::find(espmFiles.begin(), espmFiles.end(), filename) !=
    espmFiles.end();
}
