#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

// Engine-independent planning for the audited reference-local faction layer.
// A rank alone does not identify membership: an active immutable base remains
// a member under a negative override other than the explicit -1 tombstone.
namespace ActorFactions {
inline constexpr size_t kMaxEntries = 4096;
inline constexpr uint64_t kMaxRevision = 9007199254740991ULL;
struct Row
{
  uint32_t factionId = 0;
  int rank = -2;
  bool operator==(const Row&) const = default;
};
struct BaseRow
{
  uint32_t factionId = 0;
  int rank = -2;
  bool playerExpelled = false;
  bool operator==(const BaseRow&) const = default;
};
struct Entry
{
  uint32_t factionId = 0;
  int rank = -2;
  bool isMember = false;
  bool operator==(const Entry&) const = default;
};
using Rows = std::vector<Row>;
using Target = std::vector<Entry>;
using Base = std::vector<BaseRow>;

template <class T>
void Validate(const std::vector<T>& rows)
{
  if (rows.size() > kMaxEntries)
    throw std::runtime_error("Actor faction snapshot exceeds bridge limit");
  std::set<uint32_t> seen;
  for (const auto& row : rows)
    if (!row.factionId || row.rank < -128 || row.rank > 127 ||
        !seen.insert(row.factionId).second)
      throw std::runtime_error("Invalid or duplicate actor faction row");
}
inline Target Canonical(Target target)
{
  Validate(target);
  std::sort(target.begin(), target.end(), [](const auto& a, const auto& b) {
    return a.factionId < b.factionId;
  });
  return target;
}
class Layer
{
public:
  Layer(const Base& base, const Rows& overrides, bool player)
    : player(player)
  {
    Validate(base);
    Validate(overrides);
    for (const auto& row : base)
      originals.emplace(row.factionId, row);
    for (const auto& row : overrides)
      extras.emplace(row.factionId, row);
  }
  Entry Get(uint32_t id) const
  {
    const auto original = originals.find(id);
    const auto extra = extras.find(id);
    const bool baseMember =
      original != originals.end() && original->second.rank >= 0;
    const int rank = extra != extras.end() ? extra->second.rank
      : original != originals.end() &&
        !(player && original->second.playerExpelled)
      ? original->second.rank
      : -2;
    const bool member = baseMember
      ? extra == extras.end() || extra->second.rank != -1
      : extra != extras.end() && extra->second.rank >= 0;
    return { id, rank, member };
  }

private:
  std::map<uint32_t, BaseRow> originals;
  std::map<uint32_t, Row> extras;
  bool player;
};
inline Entry Effective(const Base& base, const Rows& overrides, bool player,
                       uint32_t id)
{
  return Layer(base, overrides, player).Get(id);
}
inline std::set<uint32_t> Candidates(const Base& base, const Rows& rows,
                                     const Target& target = {})
{
  std::set<uint32_t> result;
  for (const auto& row : base)
    result.insert(row.factionId);
  for (const auto& row : rows)
    result.insert(row.factionId);
  for (const auto& row : target)
    result.insert(row.factionId);
  return result;
}
inline Entry Desired(const Target& target, uint32_t id)
{
  const auto found =
    std::find_if(target.begin(), target.end(),
                 [&](const auto& row) { return row.factionId == id; });
  return found == target.end() ? Entry{ id, -2, false } : *found;
}
inline bool Matches(const Base& base, const Rows& rows, bool player,
                    const Target& target)
{
  const Layer layer(base, rows, player);
  std::map<uint32_t, Entry> desired;
  Validate(target);
  for (const auto& row : target)
    desired.emplace(row.factionId, row);
  for (auto id : Candidates(base, rows, target))
    if (layer.Get(id) !=
        (desired.contains(id) ? desired.at(id) : Entry{ id, -2, false }))
      return false;
  return true;
}
inline Rows Plan(const Base& base, const Rows& current, bool player,
                 const Target& target)
{
  Validate(base);
  Validate(current);
  Validate(target);
  if (Matches(base, current, player, target))
    return current;
  Rows result;
  std::set<uint32_t> present;
  for (const auto& entry : target) {
    result.push_back({ entry.factionId, entry.rank });
    present.insert(entry.factionId);
  }
  // An inactive immutable base can be hidden by -2 with membership false.
  // The same override over an active base would remain a member and is
  // refused.
  for (const auto& entry : base)
    if (!present.contains(entry.factionId) && entry.rank < 0)
      result.push_back({ entry.factionId, -2 });
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.factionId < b.factionId;
  });
  Validate(result);
  if (!Matches(base, result, player, target))
    throw std::runtime_error(
      "Faction rank/membership pair requires an isolated base layer");
  return result;
}
class Observations
{
public:
  void Reset(uint64_t dispatchedUpdate)
  {
    last = dispatchedUpdate;
    count = 0;
  }
  bool Match(uint64_t update)
  {
    if (update > last) {
      last = update;
      ++count;
    }
    return count >= 2;
  }

private:
  uint64_t last = 0;
  unsigned count = 0;
};
}
