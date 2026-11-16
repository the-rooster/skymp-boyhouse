#include "CampaignInventoryApi.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/LIGH.h"
#include "libespm/Utils.h"
#include <algorithm>
#include <climits>

namespace {
uint32_t Item(MpActor& actor, uint32_t id)
{
  auto& world = *actor.GetParent();
  const auto record = world.GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      !espm::utils::IsItem(record.rec->GetType()))
    throw std::runtime_error(
      "Campaign inventory currently requires a concrete base item");
  if (record.rec->GetType() == "LIGH" &&
      !(espm::Convert<espm::LIGH>(record.rec)
          ->GetData(world.GetEspmCache())
          .data.flags &
        espm::LIGH::CanBeCarried))
    throw std::runtime_error("Campaign inventory requires a carryable light");
  return id;
}
uint32_t Count(const VarValue& value)
{
  if (value.GetType() != VarValue::kType_Integer)
    throw std::invalid_argument("Inventory count must be an integer");
  const auto count = static_cast<int32_t>(value);
  if (count < 0)
    throw std::invalid_argument("Inventory count must not be negative");
  return count;
}
void Silent(const VarValue& value)
{
  if (value.GetType() != VarValue::kType_Bool)
    throw std::invalid_argument("Inventory silent argument must be a boolean");
}
uint64_t Total(const Inventory& inventory, uint32_t item)
{
  uint64_t count = 0;
  for (const auto& entry : inventory.entries)
    if (entry.baseId == item)
      count += entry.count;
  return count;
}
}

void CampaignInventoryApi::Register(VirtualMachine& vm, MpActor& actor,
                                    FormId formId,
                                    std::function<void(Event)> queueEvent)
{
  vm.RegisterFunction(
    "ObjectReference", "GetItemCount", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 1)
        throw std::invalid_argument("GetItemCount takes one item");
      auto& target = CampaignReferenceBinding::Resolve(actor, formId(self));
      const auto item = Item(actor, formId(args[0]));
      const auto total =
        Total(QuestJournal::ReadInventory(actor, target), item);
      if (total > INT_MAX)
        throw std::overflow_error("Papyrus inventory count overflow");
      return VarValue(static_cast<int32_t>(total));
    });
  vm.RegisterFunction(
    "ObjectReference", "AddItem", FunctionType::Method,
    [&, formId, queueEvent](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 3)
        throw std::invalid_argument(
          "AddItem takes item, count and silent arguments");
      const auto targetId = formId(self), item = Item(actor, formId(args[0])),
                 count = Count(args[1]);
      Silent(args[2]);
      auto& target = CampaignReferenceBinding::Resolve(actor, targetId);
      if (count) {
        if (Total(QuestJournal::ReadInventory(actor, target), item) + count >
            INT_MAX)
          throw std::overflow_error("Papyrus inventory count overflow");
        QuestJournal::EditInventory(actor, target, [&](Inventory& inventory) {
          inventory.AddItem(item, count);
        });
        queueEvent({ targetId, item, count, 0, true });
      }
      return VarValue::None();
    });
  vm.RegisterFunction(
    "ObjectReference", "RemoveItem", FunctionType::Method,
    [&, formId, queueEvent](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 4)
        throw std::invalid_argument(
          "RemoveItem takes item, count, silent and destination arguments");
      const auto sourceId = formId(self), item = Item(actor, formId(args[0])),
                 requested = Count(args[1]);
      Silent(args[2]);
      if (args[3].GetType() != VarValue::kType_Object)
        throw std::invalid_argument(
          "Inventory destination must be a reference or None");
      const auto destinationId =
        static_cast<IGameObject*>(args[3]) ? formId(args[3]) : 0;
      auto& source = CampaignReferenceBinding::Resolve(actor, sourceId);
      auto* destination = destinationId
        ? &CampaignReferenceBinding::Resolve(actor, destinationId)
        : nullptr;
      if (&source == destination || !requested)
        return VarValue::None();
      if (const auto* actorSource = source.AsActor())
        for (const auto& equipped : actorSource->GetEquipment().inv.entries)
          if (equipped.baseId == item &&
              equipped.GetWorn() != Inventory::Worn::None)
            throw std::runtime_error(
              "Equipped item removal requires transactional unequip");
      std::vector<Inventory::Entry> removed;
      uint32_t remaining = requested;
      for (const auto& entry :
           QuestJournal::ReadInventory(actor, source).entries) {
        if (entry.baseId != item || !remaining)
          continue;
        if (entry.GetWorn() != Inventory::Worn::None)
          throw std::runtime_error(
            "Equipped item removal requires transactional unequip");
        auto copy = entry;
        copy.count = std::min(remaining, entry.count);
        remaining -= copy.count;
        removed.push_back(copy);
      }
      const auto count = requested - remaining;
      if (!count)
        return VarValue::None();
      if (destination &&
          Total(QuestJournal::ReadInventory(actor, *destination), item) +
              count >
            INT_MAX)
        throw std::overflow_error("Papyrus inventory count overflow");
      QuestJournal::EditInventory(actor, source, [&](Inventory& inventory) {
        inventory.RemoveItems(removed);
      });
      if (destination) {
        for (auto& entry : removed)
          entry.SetWorn(Inventory::Worn::None);
        QuestJournal::EditInventory(
          actor, *destination,
          [&](Inventory& inventory) { inventory.AddItems(removed); });
      }
      queueEvent({ sourceId, item, count, destinationId, false });
      if (destination)
        queueEvent({ destinationId, item, count, sourceId, true });
      return VarValue::None();
    });
}
