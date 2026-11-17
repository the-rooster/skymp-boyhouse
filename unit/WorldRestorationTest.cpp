#include "FormCallbacks.h"
#include "MpActor.h"
#include "WorldState.h"
#include "save_storages/ISaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t kActor = 0x100, kTrigger = 0x200, kVisible = 0x300;
const NiPoint3 kInside{ 100, 100, 100 }, kOutside{ 200, 100, 100 };

struct Observations
{
  using Edge = std::pair<uint32_t, uint32_t>;
  std::vector<Edge> subscribed, unsubscribed;
  FormCallbacks::SubscribeCallback beforeSubscribe, beforeUnsubscribe;
  size_t messages = 0;

  FormCallbacks Callbacks()
  {
    auto result = FormCallbacks::DoNothing();
    result.subscribe = [this](auto* emitter, auto* listener) {
      if (beforeSubscribe)
        beforeSubscribe(emitter, listener);
      subscribed.emplace_back(emitter->GetFormId(), listener->GetFormId());
    };
    result.unsubscribe = [this](auto* emitter, auto* listener) {
      if (beforeUnsubscribe)
        beforeUnsubscribe(emitter, listener);
      unsubscribed.emplace_back(emitter->GetFormId(), listener->GetFormId());
    };
    result.sendToUser = [this](auto*, const auto&, bool) { ++messages; };
    result.sendToUserDeferred = [this](auto*, const auto&, bool, int, bool) {
      ++messages;
    };
    return result;
  }

  size_t Published(uint32_t emitter, uint32_t listener) const
  {
    return std::count(subscribed.begin(), subscribed.end(),
                      Edge{ emitter, listener });
  }
};

class CountingReference : public MpObjectReference
{
public:
  CountingReference(const FormCallbacks& callbacks, bool trigger)
    : MpObjectReference(LocationalData{ kInside, {}, FormDesc::Tamriel() },
                        callbacks, 0x400, trigger ? "ACTI" : "STAT")
  {
    if (trigger)
      SetPrimitive({ 10, 10, 10 });
  }

  void SendPapyrusEvent(const char* name, const VarValue*, size_t) override
  {
    ++events[name];
  }

  int Count(const std::string& name) const
  {
    const auto found = events.find(name);
    return found == events.end() ? 0 : found->second;
  }

  std::map<std::string, int> events;
};

class CountingActor : public MpActor
{
public:
  explicit CountingActor(const FormCallbacks& callbacks)
    : MpActor(LocationalData{ kInside, {}, FormDesc::Tamriel() }, callbacks)
  {
  }

  void SendPapyrusEvent(const char* name, const VarValue*, size_t) override
  {
    ++events[name];
  }

  std::map<std::string, int> events;
};

struct RestorationFixture
{
  // Outlive the world: teardown can unsubscribe references through callbacks.
  Observations observed;
  WorldState world;
  CountingActor* actor;
  CountingReference *trigger, *visible;

  RestorationFixture()
  {
    world.espmFiles = { "Skyrim.esm" };
    auto actorPtr = std::make_unique<CountingActor>(observed.Callbacks());
    actor = actorPtr.get();
    world.AddForm(std::move(actorPtr), kActor);
    auto triggerPtr =
      std::make_unique<CountingReference>(observed.Callbacks(), true);
    trigger = triggerPtr.get();
    world.AddForm(std::move(triggerPtr), kTrigger);
    auto visiblePtr =
      std::make_unique<CountingReference>(observed.Callbacks(), false);
    visible = visiblePtr.get();
    world.AddForm(std::move(visiblePtr), kVisible);
  }

  void Restore(bool actorFirst, bool disabled = false, int32_t profile = 42)
  {
    auto actorSave = actor->GetChangeForm();
    actorSave.profileId = profile;
    actorSave.isDisabled = disabled;
    const auto triggerSave = trigger->GetChangeForm();
    const auto visibleSave = visible->GetChangeForm();
    const auto loadActor = [&] {
      world.LoadChangeForm(actorSave, observed.Callbacks());
    };
    const auto loadReferences = [&] {
      world.LoadChangeForm(triggerSave, observed.Callbacks());
      world.LoadChangeForm(visibleSave, observed.Callbacks());
    };
    if (actorFirst) {
      loadActor();
      loadReferences();
    } else {
      loadReferences();
      loadActor();
    }
  }

  void RequireSilent() const
  {
    REQUIRE(actor->events.empty());
    REQUIRE(trigger->events.empty());
    REQUIRE(visible->events.empty());
    REQUIRE(observed.subscribed.empty());
    REQUIRE(observed.unsubscribed.empty());
    REQUIRE(observed.messages == 0);
  }

  void RequireLoadedOnce() const
  {
    for (const auto* ref : { trigger, visible }) {
      REQUIRE(ref->Count("OnInit") == 1);
      REQUIRE(ref->Count("OnCellLoad") == 1);
      REQUIRE(ref->Count("OnLoad") == 1);
    }
    REQUIRE(observed.Published(kVisible, kActor) == 1);
  }
};

struct CountingStorage
  : Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
  using Forms = std::vector<std::optional<MpChangeForm>>;
  std::vector<Forms> submissions;
  uint32_t ticks = 0;
  bool SupportsAtomicBatches() const noexcept override { return true; }
  bool SupportsUpsertResults() const noexcept override { return true; }
  void IterateSync(const IterateSyncCallback&) override {}
  void Iterate(const IterateCallback& callback,
               const std::optional<std::vector<FormDesc>>&) override
  {
    callback({});
  }
  void Upsert(Forms&& forms, const UpsertCallback& callback) override
  {
    submissions.push_back(std::move(forms));
    callback();
  }
  void UpsertWithResult(Forms&& forms,
                        const UpsertResultCallback& callback) override
  {
    submissions.push_back(std::move(forms));
    callback({});
  }
  uint32_t GetNumFinishedUpserts() const override
  {
    return static_cast<uint32_t>(submissions.size());
  }
  uint32_t GetNumFinishedIterates() const override { return 0; }
  void Tick() override { ++ticks; }
  bool GetRecycledChangeFormsBuffer(Forms&) override { return false; }
  const std::string& GetName() const override
  {
    static const std::string name = "restoration-counting";
    return name;
  }
};
}

TEST_CASE("Restoration seeds trigger occupancy without publishing events",
          "[WorldState][Restoration][Trigger]")
{
  const bool actorFirst = GENERATE(false, true);
  const bool explicitPromotion = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(actorFirst);
  REQUIRE_FALSE(f.world.IsRestoring());
  REQUIRE(f.actor->GetPos() == kInside);
  REQUIRE(f.trigger->IsPointInsidePrimitive(f.actor->GetPos()));
  REQUIRE(f.actor->GetEmitters().count(f.trigger) == 1);
  REQUIRE(f.trigger->GetListeners().count(f.actor) == 1);
  REQUIRE(f.trigger->GetActorListeners().empty());
  REQUIRE(f.visible->GetActorListeners().empty());
  f.RequireSilent();
  f.world.Tick(); // Restoration must not have queued a phantom Enter.
  f.RequireSilent();

  if (explicitPromotion) {
    f.actor->ForceSubscriptionsUpdate();
    f.RequireLoadedOnce();
    REQUIRE(f.trigger->Count("OnTrigger") == 0);
    REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);
    REQUIRE(f.trigger->Count("OnTriggerLeave") == 0);
  }
  // Same-grid movement must also promote pending subscriptions. The saved
  // position was already inside, so the first real boundary is Leave.
  f.actor->SetPos(kOutside);
  f.RequireLoadedOnce();
  REQUIRE(f.trigger->Count("OnTrigger") == 0);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);

  f.actor->SetPos(kInside);
  REQUIRE(f.trigger->Count("OnTrigger") == 1);
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 1);
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 1);
  f.actor->ForceSubscriptionsUpdate();
  f.trigger->ForceSubscriptionsUpdate();
  f.visible->ForceSubscriptionsUpdate();
  f.world.Tick();
  f.RequireLoadedOnce();
  REQUIRE(f.trigger->Count("OnTrigger") == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 1);
}

TEST_CASE("Restoring a moved trigger replaces its old grid and polygon",
          "[WorldState][Restoration][Trigger]")
{
  RestorationFixture f;
  f.Restore(false);
  const NiPoint3 relocated{ 10000, 100, 100 };
  const FormDesc destination{ 0xdead, "Skyrim.esm" };
  auto saved = f.trigger->GetChangeForm();
  saved.position = relocated;
  saved.worldOrCellDesc = destination;
  f.world.LoadChangeForm(saved, f.observed.Callbacks());
  REQUIRE(f.trigger->GetPos() == relocated);
  REQUIRE(f.trigger->GetCellOrWorld() == destination);
  REQUIRE_FALSE(f.trigger->IsPointInsidePrimitive(kInside));
  REQUIRE(f.trigger->IsPointInsidePrimitive(relocated));
  REQUIRE(f.world.GetNeighborsByPosition(0x3c, 0, 0).count(f.trigger) == 0);
  REQUIRE(f.world.GetNeighborsByPosition(0xdead, 2, 0).count(f.trigger) == 1);
  REQUIRE(f.actor->GetEmitters().count(f.trigger) == 0);
  f.world.Tick();
  f.RequireSilent();

  f.actor->SetCellOrWorld(destination);
  f.actor->SetPos(relocated);
  REQUIRE(f.trigger->Count("OnLoad") == 1);
  REQUIRE(f.trigger->Count("OnTrigger") == 1);
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 1);
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 0);
}

TEST_CASE("Failed reentrant subscription publication retries on real movement",
          "[WorldState][Restoration][Subscription]")
{
  RestorationFixture f;
  f.Restore(false);
  int attempts = 0;
  f.observed.beforeSubscribe = [&](auto* emitter, auto* listener) {
    if (emitter != f.visible || listener != f.actor)
      return;
    ++attempts;
    // A network callback can reenter either endpoint's spatial refresh.
    // Neither refresh may publish this in-flight edge a second time.
    emitter->ForceSubscriptionsUpdate();
    listener->ForceSubscriptionsUpdate();
    if (attempts == 1)
      throw std::runtime_error("temporary subscription failure");
  };
  REQUIRE_THROWS_WITH(f.actor->ForceSubscriptionsUpdate(),
                      "temporary subscription failure");
  REQUIRE(attempts == 1);
  REQUIRE(f.observed.Published(kVisible, kActor) == 0);
  REQUIRE(f.visible->GetListeners().count(f.actor) == 1);
  REQUIRE(f.actor->GetEmitters().count(f.visible) == 1);
  REQUIRE(f.visible->GetActorListeners().empty());
  REQUIRE(f.actor->GetPos() == kInside);
  f.world.Tick();
  REQUIRE(attempts == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);

  // A nested Force must not clear the still-pending publication flag. No
  // grid boundary is crossed, so this retry depends on that retained work.
  f.actor->SetPos(kOutside);
  REQUIRE(attempts == 2);
  f.RequireLoadedOnce();
  REQUIRE(f.visible->GetActorListeners() == std::vector<MpActor*>{ f.actor });
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);
  f.actor->ForceSubscriptionsUpdate();
  f.visible->ForceSubscriptionsUpdate();
  REQUIRE(attempts == 2);
  f.RequireLoadedOnce();
  f.observed.beforeSubscribe = {};
}

TEST_CASE("Late profile registration loads already published references once",
          "[WorldState][Restoration][Subscription]")
{
  const bool restored = GENERATE(false, true);
  const int refresh = GENERATE(0, 1, 2);
  RestorationFixture f;
  if (restored) {
    f.Restore(false, false, -1);
  } else {
    f.trigger->ForceSubscriptionsUpdate();
    f.visible->ForceSubscriptionsUpdate();
  }
  f.actor->ForceSubscriptionsUpdate();
  REQUIRE(f.actor->GetChangeForm().profileId == -1);
  REQUIRE(f.trigger->events.empty());
  REQUIRE(f.visible->events.empty());
  REQUIRE(f.observed.Published(kVisible, kActor) == 1);
  const auto subscriptions = f.observed.subscribed;
  const auto triggerListeners = f.trigger->GetActorListeners();
  const auto visibleListeners = f.visible->GetActorListeners();
  REQUIRE(triggerListeners == std::vector<MpActor*>{ f.actor });
  REQUIRE(visibleListeners == std::vector<MpActor*>{ f.actor });

  f.actor->RegisterProfileId(42);
  REQUIRE(f.trigger->events.empty());
  REQUIRE(f.visible->events.empty());
  if (refresh == 0) {
    MpObjectReference::Subscribe(f.trigger, f.actor);
    MpObjectReference::Subscribe(f.visible, f.actor);
  } else if (refresh == 1) {
    f.actor->ForceSubscriptionsUpdate();
  } else {
    f.actor->SetPos(kOutside);
  }
  f.RequireLoadedOnce();
  REQUIRE(f.observed.subscribed == subscriptions);
  REQUIRE(f.trigger->GetActorListeners() == triggerListeners);
  REQUIRE(f.visible->GetActorListeners() == visibleListeners);

  f.actor->ForceSubscriptionsUpdate();
  f.trigger->ForceSubscriptionsUpdate();
  f.visible->ForceSubscriptionsUpdate();
  f.actor->SetPos(kOutside);
  f.world.Tick();
  f.RequireLoadedOnce();
  REQUIRE(f.observed.subscribed == subscriptions);
  REQUIRE(f.observed.unsubscribed.empty());
  REQUIRE(f.trigger->GetActorListeners() == triggerListeners);
  REQUIRE(f.visible->GetActorListeners() == visibleListeners);
}

TEST_CASE("Subscription callbacks can replace their current endpoint safely",
          "[WorldState][Restoration][Subscription]")
{
  const bool destroyListener = GENERATE(false, true);
  const bool moving = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(false);
  const uint32_t removedId = destroyListener ? kActor : kVisible;
  std::weak_ptr<MpForm> removed = f.world.LookupFormByIdNoLoad(removedId);
  MpObjectReference* replacement = nullptr;
  std::map<std::string, int>* replacementEvents = nullptr;
  int calls = 0;
  f.observed.beforeSubscribe = [&](auto* emitter, auto* listener) {
    if (emitter != f.visible || listener != f.actor)
      return;
    REQUIRE(++calls == 1);
    f.world.DestroyForm(removedId);
    if (destroyListener) {
      auto next = std::make_unique<CountingActor>(f.observed.Callbacks());
      replacement = next.get();
      replacementEvents = &next->events;
      f.world.AddForm(std::move(next), removedId);
    } else {
      auto next =
        std::make_unique<CountingReference>(f.observed.Callbacks(), false);
      replacement = next.get();
      replacementEvents = &next->events;
      f.world.AddForm(std::move(next), removedId);
    }
  };
  if (moving)
    REQUIRE_NOTHROW(f.actor->SetPos(kOutside));
  else
    REQUIRE_NOTHROW(f.actor->ForceSubscriptionsUpdate());
  f.observed.beforeSubscribe = {};
  REQUIRE(calls == 1);
  // The test never pins the deleted object. Production must hold it across
  // the callback; any queued trigger lifetime can drain on the next Tick.
  REQUIRE(replacement);
  REQUIRE(f.world.LookupFormByIdNoLoad(removedId).get() == replacement);
  REQUIRE(replacement->GetListeners().empty());
  REQUIRE(replacement->GetEmitters().empty());
  REQUIRE(replacement->GetActorListeners().empty());
  REQUIRE(replacementEvents->empty());
  REQUIRE(replacement->GetPos() == kInside);
  f.world.Tick();
  REQUIRE(removed.expired());
  REQUIRE(replacementEvents->empty());
  REQUIRE(replacement->GetActorListeners().empty());
}

TEST_CASE("Subscription refresh skips a replaced later neighbour",
          "[WorldState][Restoration][Subscription]")
{
  RestorationFixture f;
  constexpr uint32_t otherId = 0x500;
  auto other =
    std::make_unique<CountingReference>(f.observed.Callbacks(), false);
  auto* otherRaw = other.get();
  f.world.AddForm(std::move(other), otherId);
  f.Restore(false);
  f.world.LoadChangeForm(otherRaw->GetChangeForm(), f.observed.Callbacks());
  // ForceSubscriptionsUpdate visits a pointer-ordered set. Select the real
  // earlier and later nonprimitive neighbours, independent of allocation.
  const bool visibleFirst =
    std::less<MpObjectReference*>{}(f.visible, otherRaw);
  auto* earlier = visibleFirst ? f.visible : otherRaw;
  auto* later = visibleFirst ? otherRaw : f.visible;
  const auto laterId = later->GetFormId();
  std::weak_ptr<MpForm> removed = f.world.LookupFormByIdNoLoad(laterId);
  CountingReference* replacement = nullptr;
  int calls = 0;
  f.observed.beforeSubscribe = [&](auto* emitter, auto* listener) {
    if (emitter != earlier || listener != f.actor)
      return;
    REQUIRE(++calls == 1);
    f.world.DestroyForm(laterId);
    auto next =
      std::make_unique<CountingReference>(f.observed.Callbacks(), false);
    replacement = next.get();
    f.world.AddForm(std::move(next), laterId);
  };
  REQUIRE_NOTHROW(f.actor->ForceSubscriptionsUpdate());
  f.observed.beforeSubscribe = {};
  REQUIRE(calls == 1);
  REQUIRE(f.observed.Published(laterId, kActor) == 0);
  REQUIRE(f.actor->GetEmitters().count(later) == 0);
  REQUIRE(replacement);
  REQUIRE(f.actor->GetEmitters().count(replacement) == 0);
  REQUIRE(replacement->GetListeners().empty());
  REQUIRE(replacement->GetActorListeners().empty());
  REQUIRE(replacement->events.empty());
  f.world.Tick();
  REQUIRE(removed.expired());
  REQUIRE(replacement->events.empty());
  // Only a subsequent explicit refresh may discover the replacement.
  replacement->ForceSubscriptionsUpdate();
  REQUIRE(f.observed.Published(laterId, kActor) == 1);
  REQUIRE(replacement->Count("OnLoad") == 1);
}

TEST_CASE("Unsubscription detaches before reentrant removal and destruction",
          "[WorldState][Restoration][Subscription]")
{
  const int destroy = GENERATE(0, 1, 2);
  RestorationFixture f;
  f.Restore(false);
  f.actor->ForceSubscriptionsUpdate();
  const auto removedId = destroy == 1 ? kVisible : kActor;
  std::weak_ptr<MpForm> removed = f.world.LookupFormByIdNoLoad(removedId);
  int calls = 0;
  f.observed.beforeUnsubscribe = [&](auto* emitter, auto* listener) {
    if (emitter != f.visible || listener != f.actor)
      return;
    REQUIRE(++calls == 1);
    REQUIRE(emitter->GetListeners().count(listener) == 0);
    REQUIRE(listener->GetEmitters().count(emitter) == 0);
    REQUIRE(emitter->GetActorListeners().empty());
    MpObjectReference::Unsubscribe(emitter, listener);
    if (destroy)
      f.world.DestroyForm(removedId);
  };
  REQUIRE_NOTHROW(MpObjectReference::Unsubscribe(f.visible, f.actor));
  f.observed.beforeUnsubscribe = {};
  REQUIRE(calls == 1);
  REQUIRE(std::count(f.observed.unsubscribed.begin(),
                     f.observed.unsubscribed.end(),
                     Observations::Edge{ kVisible, kActor }) == 1);
  if (destroy) {
    REQUIRE_FALSE(f.world.LookupFormByIdNoLoad(removedId));
  } else {
    MpObjectReference::Unsubscribe(f.visible, f.actor);
    REQUIRE(f.visible->GetListeners().count(f.actor) == 0);
    REQUIRE(f.actor->GetEmitters().count(f.visible) == 0);
    REQUIRE(f.visible->GetActorListeners().empty());
  }
  f.world.Tick();
  if (destroy)
    REQUIRE(removed.expired());
  REQUIRE(std::count(f.observed.unsubscribed.begin(),
                     f.observed.unsubscribed.end(),
                     Observations::Edge{ kVisible, kActor }) == 1);
}

TEST_CASE("Published references reject spatial restoration before mutation",
          "[WorldState][Restoration][Subscription]")
{
  const int kind = GENERATE(0, 1, 2);
  const int changed = GENERATE(0, 1, 2, 3, 4, 5);
  const bool throughWorld = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(false);
  f.actor->ForceSubscriptionsUpdate();
  f.RequireLoadedOnce();
  MpObjectReference* target = kind == 0
    ? static_cast<MpObjectReference*>(f.actor)
    : kind == 1 ? f.trigger
                : f.visible;
  const auto actorBefore = f.actor->GetChangeForm();
  const auto triggerBefore = f.trigger->GetChangeForm();
  const auto visibleBefore = f.visible->GetChangeForm();
  const auto listeners = target->GetListeners();
  const auto emitters = target->GetEmitters();
  const auto actorListeners = target->GetActorListeners();
  const auto actorEmitters = f.actor->GetEmitters();
  const auto subscribed = f.observed.subscribed;
  const auto unsubscribed = f.observed.unsubscribed;
  const auto actorEvents = f.actor->events;
  const auto triggerEvents = f.trigger->events;
  const auto visibleEvents = f.visible->events;
  const auto messages = f.observed.messages;
  auto saved = target->GetChangeForm();
  saved.displayName = "must not be partially restored";
  switch (changed) {
    case 0:
      saved.position = kOutside;
      break;
    case 1:
      saved.angle = { 0, 0, 45 };
      break;
    case 2:
      saved.worldOrCellDesc = { 0xdead, "Skyrim.esm" };
      break;
    case 3:
      saved.worldInstance = "changed-instance";
      break;
    case 4:
      saved.isDisabled = true;
      break;
    case 5:
      saved.isDeleted = true;
      break;
  }
  const auto restore = [&] {
    if (throughWorld)
      f.world.LoadChangeForm(saved, f.observed.Callbacks());
    else
      target->ApplyChangeForm(saved);
  };
  REQUIRE_THROWS_WITH(
    restore(), Catch::Matchers::ContainsSubstring("published subscriptions"));
  REQUIRE_FALSE(f.world.IsRestoring());
  REQUIRE(f.actor->GetChangeForm() == actorBefore);
  REQUIRE(f.trigger->GetChangeForm() == triggerBefore);
  REQUIRE(f.visible->GetChangeForm() == visibleBefore);
  REQUIRE(target->GetListeners() == listeners);
  REQUIRE(target->GetEmitters() == emitters);
  REQUIRE(target->GetActorListeners() == actorListeners);
  REQUIRE(f.actor->GetEmitters() == actorEmitters);
  REQUIRE(f.world.GetNeighborsByPosition(0x3c, 0, 0).count(target) == 1);
  f.world.Tick();
  REQUIRE(f.observed.subscribed == subscribed);
  REQUIRE(f.observed.unsubscribed == unsubscribed);
  REQUIRE(f.observed.messages == messages);
  REQUIRE(f.actor->events == actorEvents);
  REQUIRE(f.trigger->events == triggerEvents);
  REQUIRE(f.visible->events == visibleEvents);
  // Rejection must preserve occupancy too, including a reference's incoming
  // edges when that reference is the actor rather than the trigger.
  f.actor->SetPos(kOutside);
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);
}

TEST_CASE("Metadata restoration preserves published relationships",
          "[WorldState][Restoration][Subscription]")
{
  const bool actor = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(false);
  f.actor->ForceSubscriptionsUpdate();
  MpObjectReference* target =
    actor ? static_cast<MpObjectReference*>(f.actor) : f.visible;
  auto saved = target->GetChangeForm();
  saved.displayName = "restored metadata";
  const auto subscribed = f.observed.subscribed;
  const auto unsubscribed = f.observed.unsubscribed;
  const auto actorEvents = f.actor->events;
  const auto triggerEvents = f.trigger->events;
  const auto visibleEvents = f.visible->events;
  const auto listeners = target->GetListeners();
  const auto emitters = target->GetEmitters();
  const auto actorListeners = target->GetActorListeners();
  REQUIRE_NOTHROW(target->ApplyChangeForm(saved));
  REQUIRE(target->GetChangeForm() == saved);
  REQUIRE(target->GetListeners() == listeners);
  REQUIRE(target->GetEmitters() == emitters);
  REQUIRE(target->GetActorListeners() == actorListeners);
  f.actor->ForceSubscriptionsUpdate();
  f.world.Tick();
  REQUIRE(f.observed.subscribed == subscribed);
  REQUIRE(f.observed.unsubscribed == unsubscribed);
  REQUIRE(f.actor->events == actorEvents);
  REQUIRE(f.trigger->events == triggerEvents);
  REQUIRE(f.visible->events == visibleEvents);
  f.actor->SetPos(kOutside);
  f.world.Tick();
  REQUIRE(f.trigger->Count("OnTriggerLeave") == 1);
  REQUIRE(f.trigger->Count("OnTriggerEnter") == 0);
}

TEST_CASE("Restored actor values rebuild without sending premature updates",
          "[WorldState][Restoration][ActorValues]")
{
  RestorationFixture f;
  auto values = f.actor->GetActorValues();
  values.healthPercentage = 0.75f;
  {
    WorldState::RestorationScope restoration(f.world);
    f.actor->NetSetPercentages(values, nullptr, std::nullopt);
    REQUIRE(f.actor->GetActorValues().healthPercentage == 0.75f);
    REQUIRE(f.observed.messages == 0);
  }
  REQUIRE(f.observed.messages == 0);
  f.actor->NetSendChangeValues(f.actor->GetActorValues(), std::nullopt);
  REQUIRE(f.observed.messages == 1);
}

TEST_CASE("Disabled restored actors remain absent until enabled",
          "[WorldState][Restoration]")
{
  const bool actorFirst = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(actorFirst, true);
  REQUIRE(f.actor->IsDisabled());
  REQUIRE(f.actor->GetEmitters().empty());
  REQUIRE(f.trigger->GetListeners().empty());
  REQUIRE(f.visible->GetListeners().empty());
  f.actor->ForceSubscriptionsUpdate();
  f.world.Tick();
  f.RequireSilent();
  f.actor->Enable();
  REQUIRE_FALSE(f.actor->IsDisabled());
  REQUIRE(f.actor->GetEmitters().count(f.trigger) == 1);
  f.RequireLoadedOnce();
  f.actor->ForceSubscriptionsUpdate();
  f.RequireLoadedOnce();
}

TEST_CASE(
  "Pending restored edges detach without phantom callbacks or occupancy",
  "[WorldState][Restoration][Trigger]")
{
  const bool actorFirst = GENERATE(false, true);
  const bool destroy = GENERATE(false, true);
  RestorationFixture f;
  f.Restore(actorFirst);
  const auto triggerLifetime = f.world.LookupFormByIdNoLoad(kTrigger);
  const auto visibleLifetime = f.world.LookupFormByIdNoLoad(kVisible);
  if (destroy) {
    f.world.DestroyForm(kTrigger);
    f.world.DestroyForm(kVisible);
  } else {
    f.trigger->SetWorldInstance("elsewhere");
    f.visible->SetWorldInstance("elsewhere");
  }
  REQUIRE(f.actor->GetEmitters().count(f.trigger) == 0);
  REQUIRE(f.actor->GetEmitters().count(f.visible) == 0);
  REQUIRE(f.trigger->GetListeners().empty());
  REQUIRE(f.visible->GetListeners().empty());
  REQUIRE(f.observed.unsubscribed.empty());
  REQUIRE(f.trigger->events.empty());
  REQUIRE(f.visible->events.empty());
  f.actor->SetPos(kOutside);
  f.actor->SetPos(kInside);
  f.world.Tick();
  // Retaining the old objects makes a stale occupancy callback observable
  // without dereferencing destroyed storage in the assertion itself.
  REQUIRE(f.trigger->events.empty());
  REQUIRE(f.visible->events.empty());
  REQUIRE(f.observed.Published(kVisible, kActor) == 0);
}

TEST_CASE("Nested restoration preserves its outer guard through exceptions",
          "[WorldState][Restoration][SaveStorage]")
{
  RestorationFixture f;
  auto storage = std::make_shared<CountingStorage>();
  f.world.AttachSaveStorage(storage);
  f.visible->SetDisplayName(std::string("before restoration"));
  int regular = 0, effect = 0, nested = 0;
  f.world.SetTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++regular;
  });
  f.world.SetEffectTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
    ++effect;
  });
  {
    WorldState::RestorationScope outer(f.world);
    REQUIRE(f.world.IsRestoring());
    const auto failInner = [&] {
      WorldState::RestorationScope inner(f.world);
      f.Restore(false);
      REQUIRE(f.world.IsRestoring());
      f.world.SetTimer(std::chrono::hours(-1)).Then([&](const Viet::Void&) {
        ++nested;
      });
      f.world.Tick();
      throw std::runtime_error("interrupted restoration");
    };
    REQUIRE_THROWS_WITH(failInner(), "interrupted restoration");
    REQUIRE(f.world.IsRestoring());
    auto invalid = f.actor->GetChangeForm();
    invalid.recType = MpChangeForm::REFR;
    REQUIRE_THROWS(f.world.LoadChangeForm(invalid, f.observed.Callbacks()));
    REQUIRE(f.world.IsRestoring());
    f.visible->SetDisplayName(std::string("restored, not an ordinary edit"));
    f.world.RequestSave(*f.visible);
    f.world.Tick();
    REQUIRE(regular == 0);
    REQUIRE(effect == 0);
    REQUIRE(nested == 0);
    REQUIRE(storage->ticks == 0);
    REQUIRE(storage->submissions.empty());
    f.RequireSilent();
  }
  REQUIRE_FALSE(f.world.IsRestoring());
  REQUIRE(regular == 0);
  REQUIRE(effect == 0);
  REQUIRE(nested == 0);
  f.world.Tick();
  REQUIRE(regular == 1);
  REQUIRE(effect == 1);
  REQUIRE(nested == 1);
  REQUIRE(storage->submissions.size() == 1);
  const auto& saved = storage->submissions.front().at(f.visible->GetIdx());
  REQUIRE(saved);
  REQUIRE(saved->displayName ==
          std::optional<std::string>("before restoration"));
  f.RequireSilent();
  f.world.Tick();
  REQUIRE(regular == 1);
  REQUIRE(effect == 1);
  REQUIRE(nested == 1);

  auto invalid = f.actor->GetChangeForm();
  invalid.recType = MpChangeForm::REFR;
  REQUIRE_THROWS(f.world.LoadChangeForm(invalid, f.observed.Callbacks()));
  REQUIRE_FALSE(f.world.IsRestoring());
  f.visible->SetDisplayName(std::string("ordinary edit after failure"));
  f.world.Tick();
  REQUIRE(storage->submissions.size() == 2);
  const auto& later = storage->submissions.back().at(f.visible->GetIdx());
  REQUIRE(later);
  REQUIRE(later->displayName ==
          std::optional<std::string>("ordinary edit after failure"));
  f.actor->ForceSubscriptionsUpdate();
  f.RequireLoadedOnce();
}
