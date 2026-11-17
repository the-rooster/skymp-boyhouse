#pragma once
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Saved host work surrounding the Papyrus stack. One campaign advances one
// stage job at a time; command/operation identities survive process restarts.
struct QuestJobState
{
  struct Event
  {
    std::string target, item, container;
    uint32_t count = 0;
    bool added = false;
  };
  struct Job
  {
    uint64_t id = 0, generation = 1, segment = 0;
    std::string command, actor, owner, quest, instance, contentHash;
    uint16_t stage = 0;
    // Only begin, waiting and done are committed. A waiting cursor already
    // applied this log's condition/QSDT effects and must not repeat them.
    std::string cursor = "begin";
    bool accepted = false;
    uint32_t log = 0, instructions = 0, steps = 0;
    std::string script, function, token, awaitKey, failure;
    std::vector<Event> events;
    std::map<std::string, bool> completions;
    std::string Scope() const;
    std::string NextToken() const;
  };
  uint64_t generation = 1, nextJob = 1;
  std::optional<Job> active;
  std::vector<Job> completed;
  static constexpr size_t MaxCompleted = 256;
  static constexpr size_t MaxOperations = 256;
  static constexpr size_t MaxEvents = 4096;
  static constexpr uint32_t MaxSteps = 100000;

  static QuestJobState Read(const nlohmann::json& value);
  nlohmann::json Save() const;
  // Validate canonical heap shape plus one-to-one active job/stack ownership.
  // No object resolution or script execution occurs in this validation.
  void ValidateHeap(const nlohmann::json& heap) const;
};
