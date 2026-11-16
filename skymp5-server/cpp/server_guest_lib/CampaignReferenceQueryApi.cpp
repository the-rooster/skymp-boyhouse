#include "CampaignReferenceQueryApi.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"
#include "libespm/GroupUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace {
void NoArguments(const std::vector<VarValue>& arguments)
{
  if (!arguments.empty())
    throw std::invalid_argument("Campaign reference query takes no arguments");
}

double FiniteCoordinate(double value)
{
  if (!std::isfinite(value) ||
      std::abs(value) > std::numeric_limits<float>::max())
    throw std::runtime_error("Campaign spatial query is outside float range");
  return value;
}

const MpChangeFormREFR& Read(MpActor& actor, uint32_t original)
{
  auto& target = CampaignReferenceBinding::Resolve(actor, original);
  return QuestJournal::ReadReference(actor, target);
}

espm::LookupResult Space(MpActor& actor, const MpChangeFormREFR& state)
{
  auto& world = *actor.GetParent();
  const auto id = state.worldOrCellDesc.ToFormId(world.espmFiles);
  const auto record = world.GetEspm().GetBrowser().LookupById(id);
  if (!record.rec || record.rec->IsDeleted() ||
      (record.rec->GetType() != "CELL" && record.rec->GetType() != "WRLD"))
    throw std::runtime_error("Campaign reference has no valid cell or world");
  return record;
}

bool IsInterior(const espm::LookupResult& cell, WorldState& world)
{
  std::optional<uint16_t> flags;
  espm::RecordHeaderAccess::IterateFields(
    cell.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (std::memcmp(tag, "DATA", 4))
        return;
      // Skyrim CELL DATA occurs as both one and two bytes. Do not read past
      // the one-byte form or silently accept malformed/duplicate metadata.
      if (flags || (size != 1 && size != 2))
        throw std::runtime_error("Invalid campaign cell flags");
      uint16_t value = static_cast<uint8_t>(data[0]);
      if (size == 2)
        value |= uint16_t(static_cast<uint8_t>(data[1])) << 8;
      flags = value;
    },
    world.GetEspmCache());
  if (!flags)
    throw std::runtime_error("Campaign cell has no flags");
  return (*flags & 1) != 0;
}

std::optional<uint32_t> WorldSpaceId(MpActor& actor,
                                     const espm::LookupResult& record)
{
  if (record.rec->GetType() == "WRLD")
    return record.ToGlobalId(record.rec->GetId());
  auto& world = *actor.GetParent();
  if (IsInterior(record, world))
    return std::nullopt;
  const auto& browser = world.GetEspm().GetBrowser();
  const auto* group = espm::GetExteriorWorldGroup(browser, record.rec);
  uint32_t raw = 0;
  if (!group || !group->GetParentWRLD(raw) || !raw)
    throw std::runtime_error("Exterior cell has no parent worldspace");
  const auto id = record.ToGlobalId(raw);
  const auto parent = browser.LookupById(id);
  if (!parent.rec || parent.rec->IsDeleted() ||
      parent.rec->GetType() != "WRLD")
    throw std::runtime_error("Exterior cell parent is not a worldspace");
  return id;
}

std::pair<bool, uint32_t> CoordinateFrame(MpActor& actor,
                                         const MpChangeFormREFR& state)
{
  const auto space = Space(actor, state);
  if (const auto world = WorldSpaceId(actor, space))
    return { false, *world };
  return { true, space.ToGlobalId(space.rec->GetId()) };
}
}

void CampaignReferenceQueryApi::Register(VirtualMachine& vm, MpActor& actor,
                                         FormId formId, Resolve resolve)
{
  for (const bool angle : { false, true }) {
    for (size_t axis = 0; axis < 3; ++axis) {
      const auto name = std::string(angle ? "GetAngle" : "GetPosition") +
        static_cast<char>('X' + axis);
      vm.RegisterFunction(
        "ObjectReference", name, FunctionType::Method,
        [&, formId, angle, axis](VarValue self, std::vector<VarValue> args) {
          NoArguments(args);
          const auto& state = Read(actor, formId(self));
          return VarValue(FiniteCoordinate(
            (angle ? state.angle : state.position)[axis]));
        });
    }
  }

  vm.RegisterFunction(
    "Actor", "IsDead", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      NoArguments(args);
      const auto& state = Read(actor, formId(self));
      if (state.recType != MpChangeFormREFR::ACHR)
        throw std::invalid_argument("IsDead requires a campaign actor");
      return VarValue(state.isDead);
    });

  vm.RegisterFunction(
    "ObjectReference", "GetDistance", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      if (args.size() != 1)
        throw std::invalid_argument("GetDistance takes one reference");
      const auto& from = Read(actor, formId(self));
      const auto& to = Read(actor, formId(args[0]));
      if (CoordinateFrame(actor, from) != CoordinateFrame(actor, to))
        throw std::runtime_error(
          "Campaign distance requires the same interior or worldspace");
      const double dx = FiniteCoordinate(from.position.x) -
        FiniteCoordinate(to.position.x);
      const double dy = FiniteCoordinate(from.position.y) -
        FiniteCoordinate(to.position.y);
      const double dz = FiniteCoordinate(from.position.z) -
        FiniteCoordinate(to.position.z);
      // Subtract and square in double precision: valid float coordinates can
      // otherwise overflow before the final distance is range-checked.
      return VarValue(FiniteCoordinate(std::hypot(dx, dy, dz)));
    });

  vm.RegisterFunction(
    "ObjectReference", "GetParentCell", FunctionType::Method,
    [&, formId, resolve](VarValue self, std::vector<VarValue> args) {
      NoArguments(args);
      const auto record = Space(actor, Read(actor, formId(self)));
      if (record.rec->GetType() != "CELL")
        throw std::runtime_error(
          "Exterior GetParentCell requires a campaign cell index");
      return VarValue(resolve(record.ToGlobalId(record.rec->GetId())));
    });

  vm.RegisterFunction(
    "ObjectReference", "GetWorldSpace", FunctionType::Method,
    [&, formId, resolve](VarValue self, std::vector<VarValue> args) {
      NoArguments(args);
      const auto& state = Read(actor, formId(self));
      const auto record = Space(actor, state);
      const auto id = WorldSpaceId(actor, record);
      return id ? VarValue(resolve(*id)) : VarValue::None();
    });
}
