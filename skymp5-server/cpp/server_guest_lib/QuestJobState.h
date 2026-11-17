#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Saved host work surrounding independent Papyrus stacks. One campaign
// commits one segment at a time; job identities survive process restarts.
struct QuestJobState
{
  enum class Kind
  {
    Stage,
    Interaction,
    ReferenceLoad
  };
  enum class LoadPhase
  {
    Init,
    CellLoad,
    Load,
    Done
  };
  struct ReferenceLoad
  {
    std::string target, reference;
    std::vector<std::string> templates;
    bool operator==(const ReferenceLoad&) const = default;
  };
  struct Interaction
  {
    std::string name, target, source, targetReference, sourceReference;
    std::vector<std::string> targetTemplates, sourceTemplates;
    bool operator==(const Interaction&) const = default;
  };
  struct Event
  {
    std::string target, item, container;
    uint32_t count = 0;
    bool added = false;
    bool operator==(const Event&) const = default;
  };
  struct Timer
  {
    uint64_t issuedUnixMs = 0, dueUnixMs = 0, durationMs = 0;
    bool operator==(const Timer&) const = default;
  };
  struct Completion
  {
    enum class Kind
    {
      Boolean,
      None
    };
    Kind kind = Kind::Boolean;
    bool value = false;
    Completion() = default;
    Completion(bool result)
      : value(result)
    {
    }
    static Completion None()
    {
      Completion result;
      result.kind = Kind::None;
      return result;
    }
    bool operator==(const Completion&) const = default;
  };
  struct HandlerIdentity
  {
    nlohmann::json receiver;
    std::string rootScript, implementation, state, function;
    uint64_t sequence = 0;
    bool operator==(const HandlerIdentity&) const = default;
  };
  struct Dispatch
  {
    std::vector<nlohmann::json> recipients;
    uint32_t recipient = 0, attachment = 0;
    bool captured = false;
    std::vector<std::string> roots, templates;
    std::string reference;
    bool operator==(const Dispatch&) const = default;
  };
  struct Stage
  {
    uint64_t node = 0, child = 0;
    std::string quest, contentHash;
    uint16_t stage = 0;
    // Only begin, waiting and done are committed. A waiting cursor already
    // applied this log's condition/QSDT effects and must not repeat them.
    std::string cursor = "begin";
    bool accepted = false;
    uint32_t log = 0;
    std::string script, function, token, awaitKey, awaitKind, failure;
    std::optional<Timer> timer;
    std::optional<HandlerIdentity> handlerIdentity;
    bool operator==(const Stage&) const = default;
  };
  struct Job : Stage
  {
    Kind kind = Kind::Stage;
    std::optional<Interaction> interaction;
    bool interactionDone = true;
    std::optional<ReferenceLoad> referenceLoad;
    LoadPhase loadPhase = LoadPhase::Done;
    uint64_t id = 0, generation = 1, segment = 0, nextOperation = 1;
    std::string command, actor, owner, instance;
    std::string contentMode = "owned";
    std::string environmentHash;
    bool automatic = false;
    bool sequenced = false;
    std::optional<uint64_t> occurrence;
    uint32_t instructions = 0, steps = 0;
    std::vector<Event> events;
    uint32_t eventHead = 0;
    uint64_t nextHandler = 1;
    std::optional<Dispatch> dispatch;
    std::optional<Stage> handler;
    std::map<std::string, Completion> completions;
    std::deque<Stage> children;
    std::string Scope() const;
    std::string Scope(const Stage& stage) const;
    std::string NextToken() const;
    Stage& Current();
    const Stage& Current() const;
    bool IsComplete() const;
    bool IsPristine() const;
    const char* PrimaryEventName() const;
    bool HasPrimaryEvent() const;
    bool operator==(const Job&) const = default;
  };
  struct RetiredRange
  {
    uint64_t first = 0, last = 0;
    bool operator==(const RetiredRange&) const = default;
  };
  struct Ingress
  {
    std::string identity;
    uint64_t next = 1, retiredThrough = 0;
    std::vector<RetiredRange> retiredRanges;
    bool operator==(const Ingress&) const = default;
  };
  uint64_t generation = 1, nextJob = 1, retiredThrough = 0;
  Ingress ingress;
  std::map<uint64_t, Job> live;
  std::vector<RetiredRange> retiredRanges;
  std::vector<Job> completed;
  // All nonterminal roots, independently of the scheduler's per-tick budget.
  static constexpr size_t MaxLive = 128;
  static constexpr size_t MaxStageLive = 16;
  static constexpr size_t MaxBatchOccurrences = 64;
  static constexpr size_t MaxCompleted = 256;
  static constexpr size_t MaxRecentCompleted = 32;
  static constexpr size_t MaxRecentBytes = 1024 * 1024;
  static constexpr size_t MaxRetiredRanges =
    MaxLive + MaxRecentCompleted + MaxCompleted + 1;
  static constexpr size_t MaxIngressRetiredRanges =
    MaxLive + MaxRecentCompleted + 1;
  static constexpr size_t MaxOperations = 256;
  static constexpr size_t MaxEvents = 4096;
  static constexpr size_t MaxDepth = 128;
  static constexpr uint32_t MaxSteps = 100000;
  static constexpr size_t MaxRecipients = 1024;
  static constexpr size_t MaxAttachments = 1024;
  static constexpr size_t MaxHandlers = 4096;
  static constexpr uint64_t MaxSegments = MaxSteps;

  static QuestJobState Read(const nlohmann::json& value);
  nlohmann::json Save() const;
  // Canonical full-job JSON bytes, excluding permanent legacy completions.
  size_t RetainedSequencedBytes() const;
  // Called only at durable sequenced admission. Retire oldest sequenced work
  // until the completed tail has a free slot and is at most MaxRecentBytes.
  // Live roots do not reserve completed-result slots.
  std::vector<uint64_t> RetireForAdmission();
  // Move a finished live root to history, retaining its result and retiring
  // the oldest other numbered results to enforce MaxRecentCompleted. A newly
  // completed tail may exceed MaxRecentBytes until the next admission.
  // Both retirement operations publish only fully validated candidates.
  std::vector<uint64_t> CompleteJob(uint64_t id);
  Job* FindLive(uint64_t id);
  const Job* FindLive(uint64_t id) const;
  const Job* FindCompleted(uint64_t id) const;
  const Job* FindOccurrence(uint64_t occurrence) const;
  bool IsRetired(uint64_t id) const;
  bool IsOccurrenceRetired(uint64_t occurrence) const;
  bool HasLive() const;
  // Validate canonical heap shape plus one-to-one active job/stack ownership.
  // No object resolution or script execution occurs in this validation.
  void ValidateHeap(const nlohmann::json& heap) const;

private:
  // CompleteJob alone may validate one finished root before moving it out of
  // live. Storage and ordinary typed candidates never accept this shape.
  static QuestJobState Read(const nlohmann::json& value,
                            uint64_t completedLive);
  nlohmann::json Save(uint64_t completedLive) const;
  std::vector<uint64_t> RetireCompleted(size_t limit, bool boundBytes,
                                        uint64_t protectedId = 0);
};
