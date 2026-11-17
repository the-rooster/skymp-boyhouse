#pragma once
#include "Promise.h"
#include <chrono>
#include <cstdint>

namespace Viet {

class Timer
{
public:
  Timer();

  template <typename T>
  Promise<Void> SetTimer(T&& duration, uint32_t* outTimerId)
  {
    auto endTime = std::chrono::system_clock::now() + duration;
    return Set(endTime, outTimerId);
  };

  [[maybe_unused]] bool RemoveTimer(uint32_t timerId);
  // Cancel pending timers without resolving/rejecting their promises. Safe
  // inside a timer callback; the currently running callback is not
  // interrupted. Previous timer IDs are invalidated and may be reused
  // immediately.
  void Clear() noexcept;
  void TickTimers();

private:
  Promise<Void> Set(const std::chrono::system_clock::time_point& endTime,
                    uint32_t* outTimerId);

private:
  struct Impl;
  std::shared_ptr<Impl> pImpl;
};

}
