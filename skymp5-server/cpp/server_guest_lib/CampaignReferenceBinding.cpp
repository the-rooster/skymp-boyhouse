#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/RecordHeaderAccess.h"
#include "script_objects/MpFormGameObject.h"
#include <cstring>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

namespace {
constexpr auto kIdentity = "private.boyhouse.campaignIdentity";
std::string Identity(MpActor& owner, bool create)
{
  if (!owner.GetParent() ||
      owner.GetParent()->LookupFormByIdNoLoad(owner.GetFormId()).get() !=
        &owner)
    throw std::runtime_error("Unregistered campaign owner");
  const auto& saved = owner.GetDynamicFields().GetValueDump(kIdentity);
  if (saved == "null") {
    if (!create)
      throw std::runtime_error(
        "Campaign reference is not materialized for this character");
    std::random_device random;
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
      value << std::setw(8) << static_cast<uint32_t>(random());
    const auto identity = value.str();
    owner.SetPropertyValueDump(kIdentity, nlohmann::json(identity).dump(),
                               false, false);
    return identity;
  }
  const auto parsed = nlohmann::json::parse(saved);
  if (!parsed.is_string())
    throw std::runtime_error("Invalid saved campaign identity");
  const auto identity = parsed.get<std::string>();
  if (identity.size() != 32 ||
      identity.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("Invalid saved campaign identity");
  return identity;
}
std::string Key(MpActor& owner, uint32_t original)
{
  const auto identity = Identity(owner, false);
  auto& world = *owner.GetParent();
  if (original == 0x14 || original >= 0xff000000)
    throw std::runtime_error(
      "Campaign copy requires a static non-player original");
  return nlohmann::json{
    { "identity", identity },
    { "owner",
      FormDesc::FromFormId(owner.GetFormId(), world.espmFiles).ToString() },
    { "original", FormDesc::FromFormId(original, world.espmFiles).ToString() }
  }.dump();
}

void Validate(MpActor& owner, uint32_t original, MpObjectReference& copy)
{
  auto& world = *owner.GetParent();
  if (world.LookupFormByIdNoLoad(owner.GetFormId()).get() != &owner ||
      world.LookupFormByIdNoLoad(copy.GetFormId()).get() != &copy ||
      copy.GetParent() != &world || copy.GetFormId() < 0xff000000 ||
      copy.IsDeleted())
    throw std::runtime_error("Unregistered campaign reference copy");
  const auto prefix = "campaign:" + std::to_string(owner.GetFormId()) + ":";
  if (!owner.GetWorldInstance().starts_with(prefix) ||
      !copy.SharesWorldInstance(owner))
    throw std::runtime_error(
      "Campaign reference copy is outside the owner's instance");
  if (copy.AsActor() &&
      (copy.GetBaseId() == 7 || copy.GetChangeForm().profileId >= 0))
    throw std::runtime_error(
      "A player cannot be bound as a campaign reference copy");
  const auto object =
    std::dynamic_pointer_cast<MpFormGameObject>(copy.ToGameObject());
  if (!object || !object->ListActivePexInstances().empty())
    throw std::runtime_error(
      "A campaign copy cannot retain shared-VM script instances");
  const auto record = world.GetEspm().GetBrowser().LookupById(original);
  if (!record.rec || record.rec->IsDeleted() ||
      (record.rec->GetType() != "REFR" && record.rec->GetType() != "ACHR"))
    throw std::runtime_error(
      "Campaign original is not a live placed reference");
  if (bool(copy.AsActor()) != (record.rec->GetType() == "ACHR"))
    throw std::runtime_error(
      "Campaign reference copy has the wrong object type");
  uint32_t base = 0;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (!std::memcmp(tag, "NAME", 4)) {
        if (size != 4 || base)
          throw std::runtime_error("Malformed campaign reference base");
        std::memcpy(&base, data, 4);
      }
    },
    world.GetEspmCache());
  if (!base || copy.GetBaseId() != record.ToGlobalId(base))
    throw std::runtime_error("Campaign reference copy has the wrong base");
}

MpObjectReference* Find(MpActor& owner, uint32_t original,
                        const std::string& key)
{
  auto& world = *owner.GetParent();
  MpObjectReference* result = nullptr;
  const auto index = world.MakePrivateIndexedPropertyMapKey(
    CampaignReferenceBinding::kProperty, key);
  for (const auto id : world.GetActorsByPrivateIndexedProperty(index)) {
    const auto& form = world.LookupFormByIdNoLoad(id);
    auto* reference = form ? form->AsObjectReference() : nullptr;
    if (!reference || reference->IsDeleted() ||
        !reference->SharesWorldInstance(owner) ||
        reference->GetDynamicFields().GetValueDump(
          CampaignReferenceBinding::kProperty) != key)
      continue;
    Validate(owner, original, *reference);
    if (result)
      throw std::runtime_error("Ambiguous campaign reference copies");
    result = reference;
  }
  return result;
}
}

void CampaignReferenceBinding::Bind(MpActor& owner, uint32_t original,
                                    MpObjectReference& copy)
{
  if (!owner.GetParent())
    throw std::runtime_error("Unregistered campaign owner");
  if (&QuestJournal::Owner(owner) != &owner)
    throw std::runtime_error(
      "Only a campaign owner can bind reference copies");
  if (original == 0x14 || original >= 0xff000000)
    throw std::runtime_error(
      "Campaign copy requires a static non-player original");
  Validate(owner, original, copy);
  QuestJournal::RequireJobAccess(owner);
  Identity(owner, true);
  const auto key = Key(owner, original);
  const auto& previous = copy.GetDynamicFields().GetValueDump(kProperty);
  if (previous != "null" && previous != key)
    throw std::runtime_error("Campaign reference identity cannot be rebound");
  const auto* existing = Find(owner, original, key);
  if (existing && existing != &copy)
    throw std::runtime_error("Campaign reference already has a live copy");
  copy.RegisterPrivateIndexedProperty(kProperty, key);
}

MpObjectReference& CampaignReferenceBinding::Resolve(MpActor& actor,
                                                     uint32_t original)
{
  if (!actor.GetParent())
    throw std::runtime_error("Unregistered campaign owner");
  auto& owner = QuestJournal::Owner(actor);
  if (original == 0x14)
    return owner;
  const auto key = Key(owner, original);
  auto* copy = Find(owner, original, key);
  if (!copy)
    throw std::runtime_error(
      "Campaign reference is not materialized in this instance");
  return *copy;
}

std::pair<MpActor*, uint32_t> CampaignReferenceBinding::Context(
  MpObjectReference& copy)
{
  auto* world = copy.GetParent();
  if (!world || world->LookupFormByIdNoLoad(copy.GetFormId()).get() != &copy)
    throw std::runtime_error("Unregistered campaign reference copy");
  const auto& dump = copy.GetDynamicFields().GetValueDump(kProperty);
  const auto binding = nlohmann::json::parse(dump);
  if (!binding.is_object() || binding.size() != 3 ||
      !binding.at("owner").is_string() ||
      !binding.at("original").is_string() ||
      !binding.at("identity").is_string())
    throw std::runtime_error("Invalid campaign reference binding");
  const auto ownerId =
    FormDesc::FromString(binding.at("owner").get<std::string>())
      .ToFormId(world->espmFiles);
  const auto original =
    FormDesc::FromString(binding.at("original").get<std::string>())
      .ToFormId(world->espmFiles);
  const auto& form = world->LookupFormByIdNoLoad(ownerId);
  auto* owner = form ? form->AsActor() : nullptr;
  if (!owner || Key(*owner, original) != dump ||
      &Resolve(*owner, original) != &copy)
    throw std::runtime_error(
      "Campaign reference owner context is unavailable");
  return { owner, original };
}

uint32_t CampaignReferenceBinding::SavedOriginal(WorldState& world,
                                                 const MpChangeForm& saved)
{
  const auto binding =
    nlohmann::json::parse(saved.dynamicFields.GetValueDump(kProperty));
  if (!binding.is_object() || binding.size() != 3 ||
      !binding.at("owner").is_string() ||
      !binding.at("original").is_string() ||
      !binding.at("identity").is_string())
    throw std::runtime_error("Invalid saved campaign reference binding");
  const auto identity = binding.at("identity").get<std::string>();
  const auto owner =
    FormDesc::FromString(binding.at("owner").get<std::string>());
  const auto original =
    FormDesc::FromString(binding.at("original").get<std::string>());
  const auto ownerId = owner.ToFormId(world.espmFiles);
  const auto originalId = original.ToFormId(world.espmFiles);
  const auto copyId = saved.formDesc.ToFormId(world.espmFiles);
  const auto prefix = "campaign:" + std::to_string(ownerId) + ":";
  // Ordinary recovery reapplies effect magnitudes and resets loot. Campaign
  // copies need their own timer recovery before these states are supported.
  if (!saved.activeMagicEffects.Empty() || saved.nextRelootDatetime != 0)
    throw std::runtime_error(
      "Campaign reference timer recovery is not implemented");
  if (identity.size() != 32 ||
      identity.find_first_not_of("0123456789abcdef") != std::string::npos ||
      !owner.file.empty() || ownerId < 0xff000000 || original.file.empty() ||
      originalId == 0x14 || originalId >= 0xff000000 ||
      !saved.formDesc.file.empty() || copyId < 0xff000000 ||
      copyId == ownerId || saved.profileId != -1 ||
      saved.baseDesc.ToFormId(world.espmFiles) == 7 ||
      !saved.worldInstance.starts_with(prefix) ||
      saved.worldInstance.size() <= prefix.size() ||
      saved.worldInstance.size() > 128 || !saved.baseContainerAdded ||
      (saved.recType != MpChangeForm::REFR &&
       saved.recType != MpChangeForm::ACHR) ||
      owner.ToString() != binding.at("owner").get<std::string>() ||
      original.ToString() != binding.at("original").get<std::string>())
    throw std::runtime_error("Invalid saved campaign reference identity");
  const auto record = world.GetEspm().GetBrowser().LookupById(originalId);
  if (!record.rec || record.rec->IsDeleted() ||
      (record.rec->GetType() != "REFR" && record.rec->GetType() != "ACHR") ||
      (record.rec->GetType() == "ACHR") !=
        (saved.recType == MpChangeForm::ACHR))
    throw std::runtime_error("Invalid saved campaign original reference");
  uint32_t base = 0;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (!std::memcmp(tag, "NAME", 4)) {
        if (size != 4 || base)
          throw std::runtime_error("Malformed campaign reference base");
        std::memcpy(&base, data, 4);
      }
    },
    world.GetEspmCache());
  if (!base ||
      saved.baseDesc.ToFormId(world.espmFiles) != record.ToGlobalId(base))
    throw std::runtime_error("Saved campaign reference has the wrong base");
  return originalId;
}
