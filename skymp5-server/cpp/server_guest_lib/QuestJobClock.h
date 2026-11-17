#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Sample the clocks together. UTC is persisted for recovery; live countdowns
// use only the monotonic sample and never retain a VM or game-object pointer.
struct QuestJobTime
{
  uint64_t unixMs = 0;
  uint64_t steadyMs = 0;
};
using QuestJobClock = std::function<QuestJobTime()>;

struct QuestJobTimerAnchor
{
  std::string token;
  // Captured at the native request, including time spent awaiting its save.
  // A segment's provisional anchor is published only after AtomicJob succeeds;
  // the exact save receipt still gates eligibility to resume.
  uint64_t dueSteadyMs = 0;
};
