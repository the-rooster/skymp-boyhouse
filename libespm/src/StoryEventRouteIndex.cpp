#include "libespm/StoryEventRouteIndex.h"
#include "libespm/CombineBrowser.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace espm {
namespace {
using Kind = StoryEventRouteIndex::Kind;
using Node = StoryEventRouteIndex::Node;
using Route = StoryEventRouteIndex::Route;

template <class T>
T ReadNumber(const char* data, uint32_t size)
{
  if (size != sizeof(T))
    throw std::runtime_error("Invalid Story Manager field size");
  T result;
  std::memcpy(&result, data, size);
  return result;
}

// TES5Edit wbDefinitionsTES5: SMEN/SMBN/SMQN. FormID fields are mapped
// against the plugin containing this override, never the quest's plugin.
Node ReadNode(const LookupResult& record, CompressedFieldsCache& cache)
{
  Node n;
  const auto type = record.rec->GetType();
  n.kind = type == "SMEN" ? Kind::Event
                          : (type == "SMQN" ? Kind::Quest : Kind::Branch);
  n.id = record.ToGlobalId(record.rec->GetId());
  n.fileIndex = record.fileIdx;
  std::optional<uint32_t> conditionCount, questCount;
  QuestLifecycleData::Condition* condition = nullptr;
  StoryEventRouteIndex::Entry* entry = nullptr;
  std::unordered_set<std::string_view> seen;
  RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* name, uint32_t size, const char* bytes) {
      const std::string_view tag(name, 4);
      if (tag == "CIS1" || tag == "CIS2") {
        if (!condition || !size || bytes[size - 1] ||
            std::memchr(bytes, 0, size - 1))
          throw std::runtime_error("Invalid Story Manager condition string");
        auto& str = tag == "CIS1" ? condition->parameter1String
                                  : condition->parameter2String;
        if (str)
          throw std::runtime_error("Duplicate Story Manager condition string");
        str.emplace(bytes, size - 1);
        return;
      }
      condition = nullptr;
      if (tag == "FNAM" || tag == "RNAM") {
        if (!entry)
          throw std::runtime_error("Orphan Story Manager quest entry field");
        if (tag == "FNAM" && !entry->flags)
          entry->flags = ReadNumber<uint32_t>(bytes, size);
        else if (tag == "RNAM" && !entry->resetHours)
          entry->resetHours = ReadNumber<float>(bytes, size);
        else
          throw std::runtime_error(
            "Duplicate Story Manager quest entry field");
        return;
      }
      entry = nullptr;
      auto number = [&] { return ReadNumber<uint32_t>(bytes, size); };
      auto form = [&] {
        const auto id = number();
        return id ? record.ToGlobalId(id) : 0;
      };
      if (tag == "CTDA") {
        n.conditions.push_back({ ReadNumber<CTDA>(bytes, size), {}, {} });
        condition = &n.conditions.back();
      } else if (tag == "NNAM" && n.kind == Kind::Quest) {
        n.quests.push_back({ form(), {}, {} });
        entry = &n.quests.back();
      } else if (tag == "EDID" || tag == "XXXX") {
        return;
      } else if (tag == "PNAM" || tag == "SNAM" || tag == "CITC" ||
                 tag == "DNAM" || tag == "XNAM" ||
                 (tag == "ENAM" && n.kind == Kind::Event) ||
                 ((tag == "QNAM" || tag == "MNAM") && n.kind == Kind::Quest)) {
        if (!seen.insert(tag).second)
          throw std::runtime_error("Duplicate Story Manager singleton field");
        if (tag == "PNAM")
          n.parent = form();
        else if (tag == "SNAM")
          n.previousSibling = form();
        else if (tag == "CITC")
          conditionCount = number();
        else if (tag == "DNAM")
          n.flags = number();
        else if (tag == "XNAM")
          n.maxConcurrent = number();
        else if (tag == "ENAM")
          n.eventType = number();
        else if (tag == "QNAM")
          questCount = number();
        else
          n.questsToRun = number();
      } else {
        n.unknownFields = true;
      }
    },
    cache);
  if (!seen.count("DNAM") || !seen.count("PNAM") || !conditionCount ||
      *conditionCount != n.conditions.size() ||
      (n.kind == Kind::Event && !n.eventType) ||
      (n.kind == Kind::Quest &&
       (!questCount || *questCount != n.quests.size())))
    throw std::runtime_error("Missing or inconsistent Story Manager fields");
  return n;
}

std::string NodeGate(const Node& n)
{
  if (n.unknownFields)
    return "Story Manager node contains unknown fields";
  if (n.flags & 1)
    return "Story Manager random selection requires a draw";
  if (n.flags & ~uint32_t(3))
    return "Story Manager node flags require selection state";
  if (n.maxConcurrent || n.questsToRun.value_or(0))
    return "Story Manager quest limits require selection state";
  return {};
}
}

StoryEventRouteIndex::StoryEventRouteIndex(const CombineBrowser& browser)
{
  std::unordered_map<uint32_t, size_t> childCount, eventCount;
  for (auto type : { "SMEN", "SMBN", "SMQN" }) {
    for (const auto& record : browser.GetDistinctRecordsByType(type)) {
      const auto id = record.ToGlobalId(record.rec->GetId());
      if (record.rec->IsDeleted() || browser.LookupById(id).rec != record.rec)
        continue;
      auto node = ReadNode(record, browser.GetCache());
      ++childCount[node.parent];
      if (node.eventType)
        ++eventCount[*node.eventType];
      nodes.emplace(id, std::move(node));
    }
  }

  // SNAM is a previous sibling, not an ancestor. An absent or cyclic
  // predecessor chain must never turn an otherwise single route executable.
  const auto validSiblings = [&](const Node& first) {
    std::unordered_set<uint32_t> seen;
    auto current = &first;
    while (current->previousSibling) {
      if (!seen.insert(current->id).second)
        return false;
      current = GetNode(current->previousSibling);
      if (!current || current->parent != first.parent)
        return false;
    }
    return true;
  };

  for (const auto& [id, leaf] : nodes) {
    if (leaf.kind != Kind::Quest)
      continue;
    Route base;
    base.questNodeId = id;
    const Node* event = nullptr;
    std::unordered_set<uint32_t> seen;
    for (const Node* n = &leaf; n; n = GetNode(n->parent)) {
      if (!seen.insert(n->id).second || (n != &leaf && n->kind == Kind::Quest))
        break;
      base.nodeIds.push_back(n->id);
      if (n->kind == Kind::Event) {
        if (event)
          break;
        event = n;
      }
      if (!n->parent) {
        if (event)
          base.eventNodeId = event->id;
        break;
      }
    }
    if (!base.eventNodeId)
      continue; // Deleted/missing parent, broken chain, or no event ancestor.

    std::reverse(base.nodeIds.begin(), base.nodeIds.end());
    if (eventCount.at(*event->eventType) != 1)
      base.gate = "Multiple Story Manager nodes handle this event";
    bool belowEvent = false;
    for (const auto nodeId : base.nodeIds) {
      const auto& n = nodes.at(nodeId);
      if (base.gate.empty() && !validSiblings(n))
        base.gate = "Invalid Story Manager previous sibling chain";
      if (base.gate.empty())
        base.gate = NodeGate(n);
      if (belowEvent && base.gate.empty() && childCount.at(n.parent) > 1)
        base.gate = "Story Manager must select among sibling nodes";
      belowEvent |= nodeId == event->id;
    }
    if (base.gate.empty() && leaf.quests.size() != 1)
      base.gate = "Story Manager must select among quest entries";

    for (size_t i = 0; i < leaf.quests.size(); ++i) {
      const auto& entry = leaf.quests[i];
      const auto quest = browser.LookupById(entry.questId);
      if (!quest.rec || quest.rec->IsDeleted() ||
          quest.rec->GetType() != "QUST")
        continue;
      auto route = base;
      route.questIndex = static_cast<uint32_t>(i);
      if (route.gate.empty() &&
          (entry.flags.value_or(0) || entry.resetHours.value_or(0) != 0))
        route.gate =
          "Story Manager quest entry flags or reset interval require state";
      routes[{ entry.questId, *event->eventType }].push_back(std::move(route));
    }
  }
  for (auto& [key, matches] : routes) {
    // Diagnostic order only. Engine selection follows node and entry rules.
    std::sort(matches.begin(), matches.end(),
              [](const Route& a, const Route& b) {
                return std::tie(a.questNodeId, a.questIndex) <
                  std::tie(b.questNodeId, b.questIndex);
              });
  }
}

const StoryEventRouteIndex::Node* StoryEventRouteIndex::GetNode(
  uint32_t id) const noexcept
{
  const auto found = nodes.find(id);
  return found == nodes.end() ? nullptr : &found->second;
}

const std::vector<StoryEventRouteIndex::Route>& StoryEventRouteIndex::Find(
  uint32_t questId, uint32_t eventType) const noexcept
{
  static const std::vector<Route> empty;
  const auto found = routes.find({ questId, eventType });
  return found == routes.end() ? empty : found->second;
}
}
