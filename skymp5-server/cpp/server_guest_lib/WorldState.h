#pragma once
#include "ConditionsEvaluator.h" // ConditionsEvaluatorSettings
#include "FormIndex.h"
#include "Grid.h"
#include "GridElement.h"
#include "MpChangeForms.h"
#include "MpForm.h"
#include "MpObjectReference.h"
#include "NiPoint3.h"
#include "PartOneListener.h"
#include "SaveReceipt.h"
#include "condition_functions/ConditionFunctionMap.h"
#include "libespm/Loader.h"
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/MpFormGameObject.h"
#include <MakeID.h>
#include <Timer.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_map>

#ifdef AddForm
#  undef AddForm
#endif

class MpActor;
class FormCallbacks;
class MpChangeForm;
namespace Viet {
template <typename T, typename FormDescType, typename FilterType>
class ISaveStorage;
}
class IScriptStorage;
class GameModeEvent;
class QuestJobRunner;
class CampaignMovementPlan;

class WorldState
{
  friend class MpObjectReference;
  friend class MpActor;

public:
  using FormCallbacksFactory = std::function<FormCallbacks()>;

  class RestorationScope
  {
  public:
    explicit RestorationScope(WorldState& world);
    ~RestorationScope() noexcept;
    RestorationScope(const RestorationScope&) = delete;
    RestorationScope& operator=(const RestorationScope&) = delete;
    RestorationScope(RestorationScope&&) = delete;
    RestorationScope& operator=(RestorationScope&&) = delete;

  private:
    WorldState& world;
  };

  bool IsRestoring() const noexcept;
  bool IsClearing() const noexcept;
  // Full database reconstruction is distinct from a nested passive load.
  void BeginStorageRestore();
  void CompleteStorageRestore();
  void FailStorageRestore() noexcept;
  bool IsStorageRestoreReady() const noexcept;

  struct NpcSettingsEntry
  {
    bool spawnInInterior = false;
    bool spawnInExterior = false;
    bool overriden = false;
  };

public:
  WorldState();
  ~WorldState();
  WorldState(const WorldState&) = delete;
  WorldState& operator=(const WorldState&) = delete;

  // Quiescent reset/teardown, outside active Papyrus or game-event execution.
  // Cancels timers and clears world-derived identities; submitted saves drain
  // independently and their old receipts become Abandoned.
  void Clear();

  const std::chrono::steady_clock::time_point& GetStartPoint() const;

  void AttachEspm(espm::Loader* espm,
                  const FormCallbacksFactory& formCallbacksFactory);
  void AttachSaveStorage(
    std::shared_ptr<
      Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
      saveStorage);
  void AttachScriptStorage(std::shared_ptr<IScriptStorage> scriptStorage);

  void AddForm(std::unique_ptr<MpForm> form, uint32_t formId,
               bool skipChecks = false,
               const MpChangeForm* optionalChangeFormToApply = nullptr);

  void LoadChangeForm(const MpChangeForm& changeForm,
                      const FormCallbacks& callbacks);

  // Accepted movement is published after its save receipt, before any quest
  // timer or handler can advance the scene.
  void Tick(const std::function<void()>& afterSave = {});
  QuestJobRunner& GetQuestJobs();

  void RequestReloot(MpObjectReference& ref,
                     std::chrono::system_clock::duration time);

  void RequestSave(MpObjectReference& ref);

  struct CampaignInventoryChange
  {
    MpObjectReference* reference;
    Inventory inventory;
  };
  struct CampaignVisibilityChange
  {
    MpObjectReference* reference;
    bool disabled;
  };
  struct CampaignFactionChange
  {
    MpActor* actor;
    std::vector<Faction> factions;
  };
  // A visibility or faction commit requires a projection barrier. BeforeCommit
  // must close gameplay for every participant in the owner's private phase and
  // invalidate its packet epoch. Every BeforeCommit invocation is paired with
  // AfterCommit. True permits a fresh manifest; false means aborted or failed
  // synchronization, and must retain gates/quarantine (not a commit receipt).
  void SetCampaignVisibilityCallbacks(
    std::function<void(MpActor&)> beforeCommit,
    std::function<void(MpActor&, bool synchronized)> afterCommit);
  // Metadata-only index, cached for the attached immutable plugin set.
  bool HasCampaignEnableLinks(uint32_t original);
  // Validate complete faction snapshots against the immutable plugin set.
  void ValidateCampaignFactions(const std::vector<Faction>& factions);

private:
  friend class QuestJournal;
  friend class QuestJobRunner;
  friend class CampaignReferenceFactory;
  std::shared_ptr<const SaveReceipt> CommitCampaignReferences(
    MpActor& owner, MpChangeForm ownerState,
    const std::vector<MpChangeForm>& copies,
    const FormCallbacks* rebaseCallbacks = nullptr,
    const std::function<bool()>& stillCurrent = {});
  // Internal campaign commit: prepare every live/save copy before publishing
  // any of them. Visibility and faction changes require a projection barrier.
  std::shared_ptr<const SaveReceipt> CommitCampaignInventory(
    MpActor& owner, const std::string& journal,
    const std::vector<CampaignInventoryChange>& changes,
    const std::vector<CampaignVisibilityChange>& visibility,
    const std::vector<CampaignFactionChange>& factions,
    const std::function<void()>& validateReads, bool durable = false,
    const std::shared_ptr<CampaignMovementPlan>& movement = {});

public:
  // Storage capabilities can be supplied by the host. Call this before
  // acquiring visitor recovery state and revalidate the connection afterward.
  void RequireDurableSaveStorage() const;
  bool HasEspmFile(std::string_view filename) const noexcept;

  template <typename T>
  Viet::Promise<Viet::Void> SetTimer(T&& duration,
                                     uint32_t* outTimerId = nullptr)
  {
    return timerRegular.SetTimer(std::forward<T>(duration), outTimerId);
  }

  template <typename T>
  Viet::Promise<Viet::Void> SetEffectTimer(T&& duration,
                                           uint32_t* outTimerId = nullptr)
  {
    return timerEffects.SetTimer(std::forward<T>(duration), outTimerId);
  }

  bool RemoveTimer(uint32_t timerId);
  Viet::Promise<Viet::Void> SetTimer(
    std::reference_wrapper<const std::chrono::system_clock::time_point>
      wrapper);
  Viet::Promise<Viet::Void> SetEffectTimer(
    std::reference_wrapper<const std::chrono::system_clock::time_point>
      wrapper);
  bool RemoveEffectTimer(uint32_t timerId);

  // Loads a requested form, likely resulting in whole chunk
  // loading if not yet loaded
  const std::shared_ptr<MpForm>& LookupFormById(
    uint32_t formId, std::stringstream* optionalOutTrace = nullptr);

  // No loading
  MpForm* LookupFormByIdx(int idx);

  // No loading version of LookupFormById
  const std::shared_ptr<MpForm>& LookupFormByIdNoLoad(uint32_t formId);

  void SendPapyrusEvent(MpForm* form, const char* eventName,
                        const VarValue* arguments, size_t argumentsCount);

  const std::set<MpObjectReference*>& GetNeighborsByPosition(
    uint32_t cellOrWorld, int16_t cellX, int16_t cellY);

  std::shared_ptr<std::vector<uint32_t>> GetAllForms(uint32_t modIndex);

  // See LookupFormById comment
  template <class F>
  F& GetFormAt(uint32_t formId)
  {
    const std::shared_ptr<MpForm>& form = LookupFormById(formId);
    if (!form) {
      throw std::runtime_error(
        fmt::format("Form with id {:#x} doesn't exist", formId));
    }

    F* typedForm = nullptr;

    if constexpr (std::is_same_v<F, MpActor>) {
      typedForm = form->AsActor();
    } else if constexpr (std::is_same_v<F, MpObjectReference>) {
      typedForm = form->AsObjectReference();
    } else {
      typedForm = dynamic_cast<F*>(form.get());
    }

    if (!typedForm) {
      if constexpr (std::is_same_v<F, MpActor>) {
        if (auto ref = std::dynamic_pointer_cast<MpObjectReference>(form)) {
          auto pos = ref->GetPos();
          spdlog::warn(
            "Specified Form is ObjectReference, but we tried to treat it as "
            "Actor, likely because of a client bug. formId={:#x}, "
            "baseId={:#x}, pos is {:.2f} {:.2f} {:.2f} at {}",
            formId, ref->GetBaseId(), pos.x, pos.y, pos.z,
            ref->GetCellOrWorld().ToString());
        }
      }
      throw std::runtime_error(
        fmt::format("Form with id {:#x} is not {} (actually it is {})", formId,
                    F::Type(), MpForm::GetFormType(&*form)));
    }

    return *typedForm;
  };

  template <class FormType = MpForm>
  void DestroyForm(uint32_t formId,
                   std::shared_ptr<FormType>* outDestroyedForm = nullptr)
  {
    auto it = forms.find(formId);
    if (it == forms.end()) {
      throw std::runtime_error(
        static_cast<const std::stringstream&>(std::stringstream()
                                              << "Form with id " << std::hex
                                              << formId << " doesn't exist")
          .str());
    }

    auto& form = it->second;
    if (!dynamic_cast<FormType*>(form.get())) {
      std::stringstream s;
      s << "Expected form " << std::hex << formId << " to be "
        << MpForm::GetFormType<FormType>() << ", but got "
        << MpForm::GetFormType(form.get());
      throw std::runtime_error(s.str());
    }

    if (outDestroyedForm)
      *outDestroyedForm = std::dynamic_pointer_cast<FormType>(it->second);

    it->second->BeforeDestroy();

    if (auto formIndex = dynamic_cast<FormIndex*>(form.get())) {
      if (formIdxManager && !formIdxManager->DestroyID(formIndex->idx))
        throw std::runtime_error("DestroyID failed");
    }

    forms.erase(it);
  };

  espm::Loader& GetEspm() const;
  bool HasEspm() const;
  espm::CompressedFieldsCache& GetEspmCache();
  IScriptStorage* GetScriptStorage() const;
  VirtualMachine& GetPapyrusVm();
  const std::set<uint32_t>& GetActorsByProfileId(int32_t profileId) const;
  const std::set<uint32_t>& GetActorsByPrivateIndexedProperty(
    const std::string& privateIndexedPropertyMapKey) const;
  std::string MakePrivateIndexedPropertyMapKey(
    const std::string& propertyName,
    const std::string& propertyValueStringified);
  uint32_t GenerateFormId();
  void SetRelootTime(const std::string& recordType,
                     std::chrono::system_clock::duration time);
  std::optional<std::chrono::system_clock::duration> GetRelootTime(
    const std::string& recordType) const;

  // Utility function to check if the provided baseId has the certain keyword
  bool HasKeyword(uint32_t baseId, const char* keyword);

  // Only for tests
  auto& GetGrids() { return grids; }

  void SetNpcSettings(
    std::unordered_map<std::string, NpcSettingsEntry>&& settings);
  void SetForbiddenRelootTypes(const std::set<std::string>& types);
  void SetEnableConsoleCommandsForAllSetting(bool enable);

public:
  std::vector<std::string> espmFiles;
  std::unordered_map<int32_t, std::set<uint32_t>> actorIdByProfileId;
  std::unordered_map<std::string, std::set<uint32_t>>
    actorIdByPrivateIndexedProperty;
  std::shared_ptr<spdlog::logger> logger;
  std::vector<std::shared_ptr<PartOneListener>> listeners;
  std::unordered_map<uint32_t, uint32_t> hosters;
  std::unordered_map<uint32_t, std::map<uint32_t, float>>
    activationChildsByActivationParent;
  std::vector<std::optional<std::chrono::system_clock::time_point>>
    lastMovUpdateByIdx;

  bool isPapyrusHotReloadEnabled = false;

  bool npcEnabled = false;
  std::unordered_map<std::string, NpcSettingsEntry> npcSettings;
  NpcSettingsEntry defaultSetting;
  bool enableConsoleCommandsForAll = false;

  bool disableVanillaScriptsInExterior = true;

  std::vector<uint32_t> bannedEspmCharacterRaceIds = {
    0x000e7713, 0x00012e82, 0x001052a3, 0x00088884, 0x0008883a, 0x00088846,
    0x00108272, 0x000a82b9, 0x0008883c, 0x00088794, 0x00088845, 0x0008883d,
    0x00088844, 0x00088840, 0x000a82ba,

    /* Playable races from ArgonianRace to WoodElfRace */
    0x00013740, 0x00013741, 0x00013742, 0x00013743, 0x00013744, 0x00013745,
    0x00013746, 0x00013747, 0x00013748, 0x00013749,

    /* Mannequin */
    0x0010760a
  };

  ConditionsEvaluatorSettings conditionsEvaluatorSettings;
  ConditionFunctionMap conditionFunctionMap;
  std::vector<GameModeEvent*> currentGameModeEventsStack;

private:
  bool AttachEspmRecord(const espm::CombineBrowser& br,
                        const espm::RecordHeader* record,
                        const espm::IdMapping& mapping,
                        std::stringstream* optionalOutTrace = nullptr);

  bool LoadForm(uint32_t formId,
                std::stringstream* optionalOutTrace = nullptr);
  void TickSaveStorage(const std::chrono::system_clock::time_point& now);
  void TickTimers(const std::chrono::system_clock::time_point& now);
  [[nodiscard]] bool NpcSourceFilesOverriden() const noexcept;
  [[nodiscard]] bool IsNpcAllowed(uint32_t refrId) const noexcept;
  [[nodiscard]] uint32_t GetFileIdx(uint32_t formId) const noexcept;
  [[nodiscard]] bool IsRelootForbidden(std::string type) const noexcept;

private:
  struct GridInfo
  {
    std::shared_ptr<GridImpl<MpObjectReference*>> grid =
      std::make_shared<GridImpl<MpObjectReference*>>();
    std::map<int16_t, std::map<int16_t, bool>> loadedChunks;
  };

  std::unordered_map<uint32_t, std::shared_ptr<MpForm>> forms;
  std::unordered_map<std::string, size_t> loadOrderMap;
  std::unordered_map<uint32_t, GridInfo> grids;
  std::unique_ptr<MakeID> formIdxManager;
  std::vector<MpObjectReference*> refrByIdxUnreliable;
  espm::Loader* espm = nullptr;
  FormCallbacksFactory formCallbacksFactory;
  std::unique_ptr<espm::CompressedFieldsCache> espmCache;

  struct Impl;
  std::shared_ptr<Impl> pImpl;
  Viet::Timer timerEffects, timerRegular;
  std::chrono::steady_clock::time_point worldStartTime;
};
