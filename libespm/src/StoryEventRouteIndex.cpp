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
  std::unordered_map<uint32_t, std::vector<uint32_t>> byParent;
  for (auto type : { "SMEN", "SMBN", "SMQN" }) {
    for (const auto& record : browser.GetDistinctRecordsByType(type)) {
      const auto id = record.ToGlobalId(record.rec->GetId());
      if (record.rec->IsDeleted() || browser.LookupById(id).rec != record.rec)
        continue;
      auto node = ReadNode(record, browser.GetCache());
      byParent[node.parent].push_back(id);
      if (node.eventType)
        events[*node.eventType].push_back(id);
      for (auto& entry : node.quests) {
        const auto quest = browser.LookupById(entry.questId);
        entry.liveQuest = quest.rec && !quest.rec->IsDeleted() &&
          quest.rec->GetType() == "QUST";
      }
      nodes.emplace(id, std::move(node));
    }
  }

  // Only an unambiguous SNAM chain starting at the head establishes order.
  // Keep detached cycles, missing predecessors and forked tails separately.
  for (const auto& [parent, ids] : byParent) {
    auto& group = children[parent];
    std::unordered_map<uint32_t, std::vector<uint32_t>> next;
    for (const auto id : ids)
      next[nodes.at(id).previousSibling].push_back(id);
    std::unordered_set<uint32_t> ordered;
    uint32_t previous = 0;
    for (;;) {
      const auto found = next.find(previous);
      if (found == next.end() || found->second.size() != 1)
        break;
      previous = found->second.front();
      if (!ordered.insert(previous).second)
        break;
      group.ordered.push_back(previous);
    }
    for (const auto id : ids)
      if (!ordered.count(id))
        group.unresolved.push_back(id);
    std::sort(group.unresolved.begin(), group.unresolved.end());
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
    if (events.at(*event->eventType).size() != 1)
      base.gate = "Multiple Story Manager nodes handle this event";
    bool belowEvent = false;
    for (const auto nodeId : base.nodeIds) {
      const auto& n = nodes.at(nodeId);
      if (base.gate.empty() && !validSiblings(n))
        base.gate = "Invalid Story Manager previous sibling chain";
      if (base.gate.empty())
        base.gate = NodeGate(n);
      if (belowEvent && base.gate.empty() && byParent.at(n.parent).size() > 1)
        base.gate = "Story Manager must select among sibling nodes";
      belowEvent |= nodeId == event->id;
    }
    if (base.gate.empty() && leaf.quests.size() != 1)
      base.gate = "Story Manager must select among quest entries";

    for (size_t i = 0; i < leaf.quests.size(); ++i) {
      const auto& entry = leaf.quests[i];
      if (!entry.liveQuest)
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

StoryEventRouteIndex::Selection StoryEventRouteIndex::Select(
  uint32_t eventType, const std::function<Check(const Node&)>& evaluateNode,
  const std::function<Check(const Node&, uint32_t)>& evaluateQuest) const
{
  const auto root = events.find(eventType);
  if (root == events.end())
    return { Verdict::Reject, 0, 0, 0, {}, "No Story Manager event node" };
  if (root->second.size() != 1)
    return { Verdict::Unsupported,
             0,
             0,
             0,
             {},
             "Multiple Story Manager nodes handle this event" };

  const auto eventId = root->second.front();
  std::vector<uint32_t> path;
  std::unordered_set<uint32_t> ancestors;
  const Node* ancestor = GetNode(eventId);
  while (ancestor->parent) {
    ancestor = GetNode(ancestor->parent);
    if (!ancestor || ancestor->kind != Kind::Branch ||
        !ancestors.insert(ancestor->id).second)
      return { Verdict::Unsupported,
               eventId,
               0,
               0,
               {},
               "Invalid Story Manager event ancestry" };
    path.push_back(ancestor->id);
  }
  std::reverse(path.begin(), path.end());

  const auto nodeCheck = [&](const Node& node) -> Check {
    if (node.unknownFields)
      return { Verdict::Unsupported,
               "Story Manager node contains unknown fields" };
    return evaluateNode(node);
  };
  // Other SMENs below the structural root handle different event types.
  // Its branch ancestors can still impose conditions and scheduler rules.
  for (size_t i = 0; i < path.size(); ++i) {
    const auto& node = nodes.at(path[i]);
    auto check = nodeCheck(node);
    if (check.verdict == Verdict::Pass) {
      auto gate = NodeGate(node);
      if (gate.empty())
        continue;
      check = { Verdict::Unsupported, std::move(gate) };
    }
    path.resize(i + 1);
    return { check.verdict, node.id, 0, 0, path, check.detail };
  }

  enum class VisitMode
  {
    Ordered,
    ExcludeUnordered,
    ExcludeRandom
  };
  const auto visit = [&](auto&& self, const Node& node,
                         VisitMode mode) -> Selection {
    const auto result = [&](Verdict verdict, std::string detail = {},
                            uint32_t questId = 0, uint32_t index = 0) {
      return Selection{ verdict, node.id, questId,
                        index,   path,    std::move(detail) };
    };
    if (path.size() > 256 || (node.kind == Kind::Event && node.id != eventId))
      return result(Verdict::Unsupported, "Invalid nested Story Manager tree");
    const auto check = nodeCheck(node);
    if (check.verdict != Verdict::Pass)
      return result(check.verdict, check.detail);
    const auto gate = NodeGate(node);
    if (!gate.empty()) {
      // A draw is immaterial to exclusion if every possible choice fails.
      // Limits, event sharing, cooldowns and unknown metadata still block.
      const bool randomOnly = (node.flags & 1) &&
        !(node.flags & ~uint32_t(3)) && !node.unknownFields &&
        !node.maxConcurrent && !node.questsToRun.value_or(0);
      if (mode == VisitMode::Ordered || !randomOnly)
        return result(Verdict::Unsupported, gate);
      mode = VisitMode::ExcludeRandom;
    }

    const auto group = children.find(node.id);
    if (node.kind == Kind::Quest) {
      if (group != children.end())
        return result(Verdict::Unsupported,
                      "Invalid child of Story Manager quest node");
      for (size_t i = 0; i < node.quests.size(); ++i) {
        const auto& entry = node.quests[i];
        if (!entry.liveQuest)
          return result(Verdict::Unsupported,
                        "Missing winning Story Manager quest", entry.questId,
                        i);
        if (entry.flags.value_or(0) || entry.resetHours.value_or(0) != 0)
          return result(Verdict::Unsupported,
                        "Story Manager quest entry requires selection state",
                        entry.questId, i);
        const auto quest = evaluateQuest(node, i);
        if (mode != VisitMode::Ordered && quest.verdict == Verdict::Pass) {
          const auto* reason = mode == VisitMode::ExcludeRandom
            ? "Story Manager random selection requires a draw"
            : "Unresolved Story Manager sibling order";
          return result(Verdict::Unsupported, reason, entry.questId, i);
        }
        if (quest.verdict != Verdict::Reject)
          return result(quest.verdict, quest.detail, entry.questId, i);
      }
    } else if (group != children.end()) {
      // Subtrees without a provable sibling position may only be excluded.
      // Every possible descendant must fail its own necessary predicate.
      // An eligible or unknown entry in such a subtree stops the traversal.
      for (auto id : group->second.unresolved) {
        path.push_back(id);
        auto child = self(
          self, nodes.at(id),
          mode == VisitMode::Ordered ? VisitMode::ExcludeUnordered : mode);
        path.pop_back();
        if (child.verdict != Verdict::Reject)
          return child;
      }
      for (auto id : group->second.ordered) {
        path.push_back(id);
        auto child = self(self, nodes.at(id), mode);
        path.pop_back();
        if (child.verdict != Verdict::Reject)
          return child;
      }
    }
    return result(Verdict::Reject, "No eligible Story Manager quest in node");
  };
  path.push_back(eventId);
  return visit(visit, nodes.at(eventId), VisitMode::Ordered);
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
