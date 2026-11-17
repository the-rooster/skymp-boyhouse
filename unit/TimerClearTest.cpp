#include <Timer.h>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cstdint>
#include <utility>

using namespace std::chrono_literals;

static_assert(noexcept(std::declval<Viet::Timer&>().Clear()));

TEST_CASE("Clearing timers cancels pending callbacks without invoking them",
          "[Timer][Clear]")
{
  Viet::Timer timer;
  int resolved = 0, rejected = 0;
  uint32_t dueId = 0, futureId = 0;
  timer.SetTimer(-1h, &dueId)
    .Then([&](const Viet::Void&) { ++resolved; })
    .Catch([&](const char*) { ++rejected; });
  timer.SetTimer(1h, &futureId)
    .Then([&](const Viet::Void&) { ++resolved; })
    .Catch([&](const char*) { ++rejected; });

  timer.Clear();
  timer.Clear();
  timer.TickTimers();
  REQUIRE(resolved == 0);
  REQUIRE(rejected == 0);
  REQUIRE_FALSE(timer.RemoveTimer(dueId));
  REQUIRE_FALSE(timer.RemoveTimer(futureId));
}

TEST_CASE("Timers can reuse IDs and execute after clearing", "[Timer][Clear]")
{
  Viet::Timer timer;
  uint32_t originalId = 0, reusedId = 0;
  int cancelled = 0, completed = 0;
  timer.SetTimer(1h, &originalId).Then([&](const Viet::Void&) {
    ++cancelled;
  });
  timer.Clear();
  timer.SetTimer(-1h, &reusedId).Then([&](const Viet::Void&) { ++completed; });
  REQUIRE(reusedId == originalId);
  timer.TickTimers();
  timer.TickTimers();
  REQUIRE(cancelled == 0);
  REQUIRE(completed == 1);
}

TEST_CASE("Clearing inside a timer callback preserves newly reused IDs",
          "[Timer][Clear]")
{
  Viet::Timer timer;
  uint32_t originalId = 0, cancelledId = 0, survivorId = 0, laterId = 0,
           anotherId = 0;
  int first = 0, cancelled = 0, survivor = 0, later = 0, another = 0;
  timer.SetTimer(-2h, &originalId).Then([&](const Viet::Void&) {
    ++first;
    timer.Clear();
    timer.SetTimer(1h, &survivorId).Then([&](const Viet::Void&) {
      ++survivor;
    });
  });
  timer.SetTimer(-1h, &cancelledId).Then([&](const Viet::Void&) {
    ++cancelled;
  });

  timer.TickTimers();
  REQUIRE(first == 1);
  REQUIRE(cancelled == 0);
  REQUIRE(survivor == 0);
  REQUIRE(survivorId == originalId);

  // Finishing the old callback must not release survivorId. Otherwise this
  // next timer receives the same ID as the still-pending survivor.
  timer.SetTimer(-1h, &laterId).Then([&](const Viet::Void&) { ++later; });
  REQUIRE(laterId != survivorId);
  timer.TickTimers();
  REQUIRE(later == 1);
  REQUIRE(survivor == 0);

  timer.SetTimer(-1h, &anotherId).Then([&](const Viet::Void&) { ++another; });
  REQUIRE(anotherId == laterId);
  REQUIRE(anotherId != survivorId);
  timer.TickTimers();
  REQUIRE(another == 1);
  REQUIRE(timer.RemoveTimer(survivorId));
  REQUIRE_FALSE(timer.RemoveTimer(survivorId));
  timer.Clear();
  timer.TickTimers();
  REQUIRE(first == 1);
  REQUIRE(cancelled == 0);
  REQUIRE(survivor == 0);
}
