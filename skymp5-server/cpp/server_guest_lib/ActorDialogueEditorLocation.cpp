#include "ActorDialogueEditorLocation.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "WorldState.h"
#include "libespm/GroupUtils.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>
#include <optional>
#include <stdexcept>

namespace {
uint32_t FormId(const espm::LookupResult& record, const char* bytes)
{
  uint32_t raw;
  std::memcpy(&raw, bytes, sizeof(raw));
  return raw ? record.ToGlobalId(raw) : 0;
}

espm::LookupResult Winning(const espm::CombineBrowser& browser, uint32_t id,
                           const char* type)
{
  const auto result = browser.LookupById(id);
  if (!id || id >= 0xff000000 || !result.rec || result.rec->IsDeleted() ||
      result.rec->GetType() != type)
    throw std::runtime_error(std::string("Editor location requires a live ") +
                             type);
  return result;
}

// ACHR.XLCN names a persistence location. CELL.XLCN names the location
// assigned to the editor cell. These have different meanings.
uint32_t LocationField(const espm::LookupResult& record,
                       espm::CompressedFieldsCache& cache)
{
  std::optional<uint32_t> location;
  std::optional<bool> interior;
  const bool isCell = record.rec->GetType() == "CELL";
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (!std::memcmp(tag, "XLCN", 4)) {
        if (location || size != 4)
          throw std::runtime_error("Malformed editor location XLCN");
        location = FormId(record, data);
      } else if (isCell && !std::memcmp(tag, "DATA", 4)) {
        if (interior || (size != 1 && size != 2))
          throw std::runtime_error("Malformed editor cell flags");
        interior = (static_cast<uint8_t>(*data) & 1) != 0;
      } else if (!isCell && !std::memcmp(tag, "XLRL", 4) &&
                 (size != 4 || FormId(record, data))) {
        throw std::runtime_error(
          "ACHR location reference override is unsupported");
      }
    },
    cache);
  if (isCell && !interior.value_or(false))
    throw std::runtime_error("Exterior editor locations are unsupported");
  if (!location.value_or(0))
    throw std::runtime_error(isCell
                               ? "Editor cell has no assigned location"
                               : "Original ACHR has no persistence location");
  return *location;
}

// TES5Edit wbDefinitionsTES5.pas, LCTN: LCUN and ACUN are triples of
// NPC_, ACHR, and LCTN. CommonLibSSE-NG RE/B/BGSLocation.h names the third
// member of UniqueNPCData editorLoc. RCUN is keyed by actor base.
bool ConfirmUniqueRow(const espm::LookupResult& table, uint32_t original,
                      uint32_t base, uint32_t editorLocation,
                      espm::CompressedFieldsCache& cache)
{
  bool found = false;
  espm::RecordHeaderAccess::IterateFields(
    table.rec,
    [&](const char* tag, uint32_t size, const char* data) {
      if (!std::memcmp(tag, "RCUN", 4)) {
        if (size % 4)
          throw std::runtime_error("Malformed removed unique-actor table");
        for (size_t i = 0; i < size; i += 4) {
          if (FormId(table, data + i) == base)
            throw std::runtime_error(
              "Unique actor has a location removal override");
        }
      } else if (!std::memcmp(tag, "ACUN", 4) ||
                 !std::memcmp(tag, "LCUN", 4)) {
        if (size % 12)
          throw std::runtime_error(
            "Malformed unique-actor editor location table");
        for (size_t i = 0; i < size; i += 12) {
          const auto rowBase = FormId(table, data + i);
          const auto rowRef = FormId(table, data + i + 4);
          if (rowBase != base && rowRef != original)
            continue;
          if (rowBase != base || rowRef != original ||
              FormId(table, data + i + 8) != editorLocation)
            throw std::runtime_error(
              "Unique-actor entry conflicts with original editor placement");
          found = true;
        }
      }
    },
    cache);
  return found;
}
}

uint32_t ActorDialogueEditorLocation(MpActor& actor)
{
  if (actor.GetDynamicFields().GetValueDump(
        CampaignReferenceBinding::kProperty) == "null")
    throw std::runtime_error("Actor has no bound original editor reference");
  uint32_t original;
  try {
    original = CampaignReferenceBinding::Context(actor).second;
  } catch (const std::exception& error) {
    throw std::runtime_error(
      std::string("Invalid original reference binding: ") + error.what());
  }
  auto& world = *actor.GetParent();
  const auto& browser = world.GetEspm().GetBrowser();
  auto& cache = world.GetEspmCache();
  const auto placed = Winning(browser, original, "ACHR");
  const auto container = LocationField(placed, cache);
  const auto location = Winning(browser, container, "LCTN");
  if (espm::GetExteriorWorldGroup(browser, placed.rec))
    throw std::runtime_error("Exterior editor locations are unsupported");
  const auto cellGroup = espm::GetCellGroup(browser, placed.rec);
  uint32_t rawCell;
  if (!cellGroup || !cellGroup->GetParentCELL(rawCell) || !rawCell)
    throw std::runtime_error("Original ACHR has no editor cell");
  const auto cell = Winning(browser, placed.ToGlobalId(rawCell), "CELL");
  const auto editorLocation = LocationField(cell, cache);
  Winning(browser, editorLocation, "LCTN");

  // Some overrides contain only deltas. Accept an explicit matching winning
  // row; leave reconciling missing, removed, or conflicting rows to a full
  // location override merger.
  bool confirmed = false;
  for (const auto& layer : browser.LookupByIdAll(container)) {
    if (layer.rec->IsDeleted() || layer.rec->GetType() != "LCTN")
      throw std::runtime_error("Unsupported persistence location override");
    const bool match = ConfirmUniqueRow(layer, original, actor.GetBaseId(),
                                        editorLocation, cache);
    if (layer.rec == location.rec)
      confirmed = match;
  }
  if (!confirmed)
    throw std::runtime_error(
      "Winning persistence location has no unique-actor editor entry");
  return editorLocation;
}
