#include "CampaignReferenceFactory.h"
#include "CampaignReferenceBinding.h"
#include "EvaluateTemplate.h"
#include "GetBaseActorValues.h"
#include "InventoryDefaults.h"
#include "LeveledListUtils.h"
#include "LocationalDataUtils.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "QuestVmState.h"
#include "WorldState.h"
#include "libespm/GroupUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace {
using Json = nlohmann::json;
constexpr auto kIdentity = "private.boyhouse.campaignIdentity";

std::string Destination(MpActor& owner, const std::string& areaId)
{
  if (areaId.empty() || areaId.size() > 48 ||
      areaId.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
        std::string::npos)
    throw std::invalid_argument("Invalid campaign allocation area ID");
  return "campaign:" + std::to_string(owner.GetFormId()) + ":" + areaId;
}

std::string Identity(const DynamicFields& fields)
{
  const auto& dump = fields.GetValueDump(kIdentity);
  if (dump == "null") {
    std::random_device random;
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
      value << std::setw(8) << static_cast<uint32_t>(random());
    return value.str();
  }
  const auto value = Json::parse(dump).get<std::string>();
  if (value.size() != 32 ||
      value.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("Invalid campaign allocation identity");
  return value;
}

struct Allocation
{
  MpActor& owner;
  WorldState& world;
  const espm::CombineBrowser& browser;
  std::string identity;
  std::string instance;
  Json ledger;
  std::map<uint32_t, bool> preparedDisabled;
  uint32_t level;
  QuestVmState vm;
  std::mt19937 random{ std::random_device{}() };
  LeveledListUtils::Options options;

  Allocation(MpActor& actor, const std::string& destination,
             uint32_t encounterLevel, bool readVm = true)
    : owner(actor)
    , world(*actor.GetParent())
    , browser(world.GetEspm().GetBrowser())
    , identity(Identity(actor.GetDynamicFields()))
    , instance(destination)
    , level(encounterLevel)
    , vm(readVm ? QuestJournal::PersonalScriptState(actor) : Json::object(),
         {}, {})
  {
    const auto& saved = owner.GetDynamicFields().GetValueDump(
      CampaignReferenceFactory::kAllocations);
    ledger = saved == "null"
      ? Json{ { "schemaVersion", 1 }, { "copies", Json::object() } }
      : Json::parse(saved);
    if (!ledger.is_object() || ledger.size() != 2 ||
        ledger.at("schemaVersion") != 1 || !ledger.at("copies").is_object() ||
        ledger.at("copies").size() > 100000)
      throw std::runtime_error("Invalid campaign allocation ledger");
    for (const auto& [original, copy] : ledger.at("copies").items()) {
      const auto source = FormDesc::FromString(original);
      if (source.file.empty() || source.ToString() != original ||
          !copy.is_string())
        throw std::runtime_error("Invalid campaign allocation entry");
      const auto target = FormDesc::FromString(copy.get<std::string>());
      if (!target.file.empty() ||
          target.ToFormId(world.espmFiles) < 0xff000000 ||
          target.ToString() != copy.get<std::string>())
        throw std::runtime_error("Invalid campaign allocation target");
    }
    options.random = &random;
    options.readGlobal = [&](uint32_t id) {
      const auto key = FormDesc::FromFormId(id, world.espmFiles).ToString();
      if (const auto value = vm.GetGlobal(key))
        return *value;
      const auto record = browser.LookupById(id);
      std::optional<float> value;
      espm::RecordHeaderAccess::IterateFields(
        record.rec,
        [&](const char* tag, uint32_t size, const char* data) {
          if (std::memcmp(tag, "FLTV", 4))
            return;
          if (size != 4 || value)
            throw std::runtime_error("Invalid allocation global");
          float decoded;
          std::memcpy(&decoded, data, 4);
          value = decoded;
        },
        world.GetEspmCache());
      if (!value || !std::isfinite(*value))
        throw std::runtime_error("Invalid allocation global");
      return *value;
    };
  }

  std::string OriginalKey(uint32_t original) const
  {
    return FormDesc::FromFormId(original, world.espmFiles).ToString();
  }
  std::string Binding(uint32_t original) const
  {
    return Json{
      { "identity", identity },
      { "owner",
        FormDesc::FromFormId(owner.GetFormId(), world.espmFiles).ToString() },
      { "original", OriginalKey(original) }
    }.dump();
  }
  MpObjectReference* Existing(uint32_t original,
                              bool requireCurrentArea = true)
  {
    const auto key = OriginalKey(original);
    const auto binding = Binding(original);
    const auto& indexed = world.GetActorsByPrivateIndexedProperty(
      world.MakePrivateIndexedPropertyMapKey(
        CampaignReferenceBinding::kProperty, binding));
    if (indexed.size() > 1)
      throw std::runtime_error("Duplicate allocated campaign references");
    std::optional<uint32_t> id;
    if (ledger.at("copies").contains(key))
      id = FormDesc::FromString(ledger["copies"][key].get<std::string>())
             .ToFormId(world.espmFiles);
    else if (!indexed.empty())
      id = *indexed.begin(); // Adopt a previously explicitly bound copy.
    if (!id)
      return nullptr;
    const auto form = world.LookupFormByIdNoLoad(*id);
    auto* reference = form ? form->AsObjectReference() : nullptr;
    if (!reference || indexed.size() != 1 || *indexed.begin() != *id ||
        reference->GetDynamicFields().GetValueDump(
          CampaignReferenceBinding::kProperty) != binding ||
        CampaignReferenceBinding::SavedOriginal(
          world, reference->GetChangeForm()) != original)
      throw std::runtime_error(
        "Allocated campaign reference is missing or inconsistent");
    // Do not reroll a deleted copy or a copy already assigned to another area.
    // Cross-area reference migration must be an explicit world transaction.
    if (requireCurrentArea && reference->GetWorldInstance() != instance)
      throw std::runtime_error(
        "Allocated reference belongs to another campaign area");
    return reference;
  }

  bool Disabled(uint32_t original, std::set<uint32_t>& path)
  {
    if (path.size() >= 128 || !path.insert(original).second)
      throw std::runtime_error("Cyclic or excessive enable-parent chain");
    if (const auto prepared = preparedDisabled.find(original);
        prepared != preparedDisabled.end()) {
      path.erase(original);
      return prepared->second;
    }
    if (auto* existing = Existing(original, false)) {
      path.erase(original);
      return existing->IsDisabled() || existing->IsDeleted();
    }
    const auto source = browser.LookupById(original);
    if (!source.rec || source.rec->IsDeleted() ||
        (source.rec->GetType() != "REFR" && source.rec->GetType() != "ACHR"))
      throw std::runtime_error("Invalid campaign enable parent");
    uint32_t parent = 0, flags = 0;
    bool found = false;
    espm::RecordHeaderAccess::IterateFields(
      source.rec,
      [&](const char* tag, uint32_t size, const char* data) {
        if (std::memcmp(tag, "XESP", 4))
          return;
        if (found || size != 8)
          throw std::runtime_error("Invalid enable-parent data");
        found = true;
        std::memcpy(&parent, data, 4);
        flags =
          static_cast<uint8_t>(data[4]); // Three trailing bytes are unused.
        if (flags & ~3u)
          throw std::runtime_error("Unsupported enable-parent flags");
      },
      world.GetEspmCache());
    const bool disabled = parent
      ? (Disabled(source.ToGlobalId(parent), path) != bool(flags & 1u))
      : bool(source.rec->GetFlags() & 0x800u);
    path.erase(original);
    return disabled;
  }

  MpChangeForm Defaults(uint32_t original)
  {
    const auto source = browser.LookupById(original);
    if (original == 0x14 || original >= 0xff000000 || !source.rec ||
        source.rec->IsDeleted() ||
        (source.rec->GetType() != "REFR" && source.rec->GetType() != "ACHR"))
      throw std::runtime_error("Expected a live placed campaign reference");
    const bool actor = source.rec->GetType() == "ACHR";
    const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                        ->GetData(world.GetEspmCache());
    if (!data.loc || !data.baseId)
      throw std::runtime_error("Campaign reference has no placement/base");
    const auto base = browser.LookupById(source.ToGlobalId(data.baseId));
    if (!base.rec || base.rec->IsDeleted() ||
        (actor && base.rec->GetType() != "NPC_"))
      throw std::runtime_error("Invalid campaign reference base");
    const auto cell =
      source.ToGlobalId(espm::GetWorldOrCell(browser, source.rec));
    const auto cellRecord = browser.LookupById(cell);
    if (!cell || !cellRecord.rec || cellRecord.rec->IsDeleted() ||
        (cellRecord.rec->GetType() != "CELL" &&
         cellRecord.rec->GetType() != "WRLD"))
      throw std::runtime_error("Invalid campaign reference location");
    MpChangeForm result;
    result.recType = actor ? MpChangeForm::ACHR : MpChangeForm::REFR;
    result.baseDesc =
      FormDesc::FromFormId(source.ToGlobalId(data.baseId), world.espmFiles);
    result.worldOrCellDesc = FormDesc::FromFormId(cell, world.espmFiles);
    result.worldInstance = instance;
    result.position = LocationalDataUtils::GetPos(data.loc);
    result.angle = LocationalDataUtils::GetRot(data.loc);
    for (const auto value :
         { result.position.x, result.position.y, result.position.z,
           result.angle.x, result.angle.y, result.angle.z })
      if (!std::isfinite(value))
        throw std::runtime_error("Non-finite campaign placement");
    if (std::abs(result.position.x) > 60000000 ||
        std::abs(result.position.y) > 60000000)
      throw std::runtime_error("Campaign placement exceeds grid bounds");
    std::set<uint32_t> path;
    result.isDisabled = Disabled(original, path);
    result.isDead =
      actor && reinterpret_cast<const espm::ACHR*>(source.rec)->StartsDead();
    result.count = data.count;
    result.baseContainerAdded = true;
    const auto baseId = result.baseDesc.ToFormId(world.espmFiles);
    if (actor) {
      for (const auto id : LeveledListUtils::EvaluateTemplateChain(
             browser, base, level, &options))
        result.templateChain.push_back(
          FormDesc::FromFormId(id, world.espmFiles));
      result.actorValues =
        GetBaseActorValues(&world, baseId, 0, result.templateChain);
      if (result.isDead)
        result.actorValues.healthPercentage = 0;
      result.factions = EvaluateTemplate<espm::NPC_::UseFactions>(
        &world, baseId, result.templateChain,
        [&](const auto& record, const auto& npc) {
          std::vector<Faction> factions;
          for (const auto& faction : npc.factions)
            factions.push_back(
              { FormDesc::FromFormId(record.ToGlobalId(faction.formId),
                                     world.espmFiles),
                faction.rank });
          return factions;
        });
      const auto spells = EvaluateTemplate<espm::NPC_::UseSpelllist>(
        &world, baseId, result.templateChain,
        [&](const auto& record, const auto& npc) {
          std::vector<uint32_t> ids;
          for (const auto id : npc.spells)
            ids.push_back(record.ToGlobalId(id));
          return ids;
        });
      for (const auto id : spells)
        for (const auto& [spell, count] :
             LeveledListUtils::EvaluateListRecurse(
               browser, browser.LookupById(id), 1, level, nullptr, &options))
          result.learnedSpells.LearnSpell(spell);
    }
    auto expand = [&](const espm::CONT::ContainerObject& entry) {
      return LeveledListUtils::EvaluateListRecurse(
        browser, browser.LookupById(entry.formId), entry.count, level, nullptr,
        &options);
    };
    for (const auto& entry :
         GetInventoryObjects(&world, result.templateChain, base))
      for (const auto& [id, count] : expand(entry))
        result.inv.AddItem(id, count);
    for (const auto& entry :
         GetOutfitObjects(&world, result.templateChain, base)) {
      for (const auto& [id, count] : expand(entry)) {
        result.inv.AddItem(id, count);
        Inventory::Entry worn(id, count);
        worn.SetWorn(Inventory::Worn::Right);
        result.equipment.inv.AddItems({ worn });
      }
    }
    if (actor && result.isDead) {
      const auto deathItem = EvaluateTemplate<espm::NPC_::UseTraits>(
        &world, baseId, result.templateChain,
        [](const auto& record, const auto& npc) {
          return record.ToGlobalId(npc.deathItem);
        });
      if (deathItem)
        for (const auto& [id, count] : expand({ deathItem, 1 }))
          result.inv.AddItem(id, count);
    }
    // Match the existing spawn policy for a best inventory weapon, using the
    // same selected inventory rather than rolling any list a second time.
    if (actor) {
      const Inventory::Entry* best = nullptr;
      int damage = -1;
      for (const auto& entry : result.inv.entries) {
        if (const auto* weapon = espm::Convert<espm::WEAP>(
              browser.LookupById(entry.baseId).rec)) {
          const auto data = weapon->GetData(world.GetEspmCache());
          if (!data.weapData)
            throw std::runtime_error("Missing campaign weapon data");
          const auto value = data.weapData->damage;
          if (value > damage) {
            best = &entry;
            damage = value;
          }
        }
      }
      std::erase_if(result.equipment.inv.entries, [&](const auto& entry) {
        return browser.LookupById(entry.baseId).rec->GetType() == "WEAP";
      });
      if (best) {
        auto worn = *best;
        worn.SetWorn(Inventory::Worn::Right);
        result.equipment.inv.AddItems({ worn });
      }
    }
    result.dynamicFields.SetValueDump(CampaignReferenceBinding::kProperty,
                                      Binding(original));
    return result;
  }
};
}

Json CampaignReferenceFactory::Snapshot(MpActor& actor)
{
  if (!actor.GetParent() || actor.IsDeleted() ||
      actor.GetParent()->LookupFormByIdNoLoad(actor.GetFormId()).get() !=
        &actor)
    throw std::runtime_error("Invalid campaign projection actor");
  auto& owner = QuestJournal::Owner(actor);
  const auto& instance = actor.GetWorldInstance();
  Json result{ { "schemaVersion", 1 },
               { "actor", actor.GetFormId() },
               { "campaignOwner", owner.GetFormId() },
               { "instance", instance },
               { "contextEpoch",
                 std::to_string(actor.GetWorldInstanceEpoch()) + ":" +
                   std::to_string(owner.GetWorldInstanceEpoch()) },
               { "identity", "" },
               { "references", Json::array() } };
  if (instance.empty())
    return result;
  const auto& fields = owner.GetDynamicFields();
  if (fields.GetValueDump(kAllocations) == "null") {
    if (fields.GetValueDump(kIdentity) != "null")
      result["identity"] = Identity(fields);
    return result;
  }
  // Never generate an identity in response to a read. A nonempty allocation
  // ledger without its original identity is corrupt, not a new campaign.
  if (fields.GetValueDump(kIdentity) == "null")
    throw std::runtime_error("Campaign allocation identity is missing");
  Allocation allocation(owner, instance, 1, false);
  result["identity"] = allocation.identity;
  for (const auto& [key, target] : allocation.ledger.at("copies").items()) {
    const auto original =
      FormDesc::FromString(key).ToFormId(allocation.world.espmFiles);
    auto* copy = allocation.Existing(original, false);
    if (!copy || copy->GetWorldInstance() != instance)
      continue;
    auto& references = result["references"];
    if (references.size() == 4096)
      throw std::runtime_error("Campaign projection exceeds area limit");
    references.push_back({ { "original", original },
                           { "copy", copy->GetFormId() },
                           { "baseId", copy->GetBaseId() },
                           { "kind", copy->AsActor() ? "actor" : "object" },
                           { "disabled", copy->IsDisabled() },
                           { "deleted", copy->IsDeleted() } });
  }
  return result;
}

std::vector<uint32_t> CampaignReferenceFactory::Ensure(
  MpActor& owner, const std::vector<uint32_t>& originals,
  uint32_t encounterLevel)
{
  const auto prefix = "campaign:" + std::to_string(owner.GetFormId()) + ":";
  if (!owner.GetWorldInstance().starts_with(prefix) ||
      owner.GetWorldInstance().size() <= prefix.size())
    throw std::runtime_error(
      "Campaign allocation requires the owning private phase");
  return EnsureForInstance(owner, owner.GetWorldInstance(), originals,
                           encounterLevel);
}

std::vector<uint32_t> CampaignReferenceFactory::Prepare(
  MpActor& owner, const std::string& areaId,
  const std::vector<uint32_t>& originals, uint32_t encounterLevel)
{
  return EnsureForInstance(owner, Destination(owner, areaId), originals,
                           encounterLevel);
}

std::vector<uint32_t> CampaignReferenceFactory::EnsureForInstance(
  MpActor& owner, const std::string& instance,
  const std::vector<uint32_t>& originals, uint32_t encounterLevel)
{
  if (!owner.GetParent() || QuestJournal::HasActiveTransaction() ||
      owner.GetParent()->LookupFormByIdNoLoad(owner.GetFormId()).get() !=
        &owner ||
      owner.GetFormId() < 0xff000000 || owner.IsDeleted() || !encounterLevel ||
      encounterLevel > 65535 || originals.size() > 4096)
    throw std::runtime_error("Invalid campaign allocation request");
  if (originals.empty())
    return {};
  Allocation allocation(owner, instance, encounterLevel);
  std::map<uint32_t, uint32_t> ids;
  std::vector<MpChangeForm> copies;
  for (const auto original : originals) {
    if (ids.contains(original))
      continue;
    uint32_t id;
    if (auto* existing = allocation.Existing(original)) {
      id = existing->GetFormId();
    } else {
      auto saved = allocation.Defaults(original);
      allocation.preparedDisabled.emplace(original, saved.isDisabled);
      id = allocation.world.GenerateFormId();
      if (id < 0xff000000)
        throw std::runtime_error("Campaign dynamic ID space exhausted");
      saved.formDesc = FormDesc::FromFormId(id, allocation.world.espmFiles);
      copies.push_back(std::move(saved));
    }
    ids.emplace(original, id);
    allocation.ledger["copies"][allocation.OriginalKey(original)] =
      FormDesc::FromFormId(id, allocation.world.espmFiles).ToString();
  }
  const auto dump = allocation.ledger.dump();
  if (dump.size() > 8 * 1024 * 1024 ||
      allocation.ledger["copies"].size() > 100000)
    throw std::runtime_error("Campaign allocation ledger exceeds limit");
  auto state = owner.GetChangeForm();
  state.dynamicFields.SetValueDump(kIdentity,
                                   Json(allocation.identity).dump());
  state.dynamicFields.SetValueDump(kAllocations, dump);
  // Prepare the return value too: no allocation is allowed to make a
  // successful commit appear to have failed to its caller.
  std::vector<uint32_t> result;
  for (const auto original : originals)
    result.push_back(ids.at(original));
  if (!copies.empty() || state.dynamicFields != owner.GetDynamicFields())
    allocation.world.CommitCampaignReferences(owner, std::move(state), copies);
  return result;
}

std::vector<uint32_t> CampaignReferenceFactory::EnsureArea(
  MpActor& owner, uint32_t worldOrCell, const NiPoint3& center, float radius,
  uint32_t encounterLevel)
{
  const auto prefix = "campaign:" + std::to_string(owner.GetFormId()) + ":";
  if (!owner.GetWorldInstance().starts_with(prefix) ||
      owner.GetWorldInstance().size() <= prefix.size())
    throw std::runtime_error(
      "Campaign allocation requires the owning private phase");
  return EnsureAreaForInstance(owner, owner.GetWorldInstance(), worldOrCell,
                               center, radius, encounterLevel);
}

std::vector<uint32_t> CampaignReferenceFactory::PrepareArea(
  MpActor& owner, const std::string& areaId, uint32_t worldOrCell,
  const NiPoint3& center, float radius, uint32_t encounterLevel)
{
  return EnsureAreaForInstance(owner, Destination(owner, areaId), worldOrCell,
                               center, radius, encounterLevel);
}

std::vector<uint32_t> CampaignReferenceFactory::EnsureAreaForInstance(
  MpActor& owner, const std::string& instance, uint32_t worldOrCell,
  const NiPoint3& center, float radius, uint32_t encounterLevel)
{
  if (!owner.GetParent() || !std::isfinite(radius) || radius <= 0 ||
      radius > 65536 || !std::isfinite(center.x) || !std::isfinite(center.y) ||
      !std::isfinite(center.z) || std::abs(center.x) + radius > 60000000 ||
      std::abs(center.y) + radius > 60000000)
    throw std::runtime_error("Invalid campaign allocation area");
  auto& world = *owner.GetParent();
  const auto& browser = world.GetEspm().GetBrowser();
  const auto cell = browser.LookupById(worldOrCell);
  if (!cell.rec || cell.rec->IsDeleted() ||
      (cell.rec->GetType() != "CELL" && cell.rec->GetType() != "WRLD"))
    throw std::runtime_error("Invalid campaign allocation world/cell");
  std::set<uint32_t> originals;
  for (size_t file = 0; file < world.espmFiles.size(); ++file) {
    const auto rawCell =
      espm::utils::GetMappedId(worldOrCell, *browser.GetRawMapping(file));
    for (int x = int((center.x - radius) / 4096);
         x <= int((center.x + radius) / 4096); ++x) {
      for (int y = int((center.y - radius) / 4096);
           y <= int((center.y + radius) / 4096); ++y) {
        const auto records =
          browser.GetRecordsAtPos(rawCell, int16_t(x), int16_t(y));
        for (const auto* candidate : *records[file]) {
          const auto id = espm::utils::GetMappedId(
            candidate->GetId(), *browser.GetCombMapping(file));
          if (id == 0x14)
            continue;
          const auto source = browser.LookupById(id);
          if (!source.rec || source.rec->IsDeleted() ||
              source.ToGlobalId(espm::GetWorldOrCell(browser, source.rec)) !=
                worldOrCell)
            continue;
          if (source.rec->GetType() != "REFR" &&
              source.rec->GetType() != "ACHR")
            throw std::runtime_error(
              "Campaign area reference changed record type");
          const auto data = reinterpret_cast<const espm::REFR*>(source.rec)
                              ->GetData(world.GetEspmCache());
          if (!data.loc)
            throw std::runtime_error(
              "Campaign area reference has no placement");
          const double dx = double(data.loc->pos[0]) - center.x;
          const double dy = double(data.loc->pos[1]) - center.y;
          const double dz = double(data.loc->pos[2]) - center.z;
          if (dx * dx + dy * dy + dz * dz <= double(radius) * radius)
            originals.insert(id);
          if (originals.size() > 4096)
            throw std::runtime_error(
              "Campaign area allocation exceeds batch limit");
        }
      }
    }
  }
  return EnsureForInstance(
    owner, instance, { originals.begin(), originals.end() }, encounterLevel);
}
