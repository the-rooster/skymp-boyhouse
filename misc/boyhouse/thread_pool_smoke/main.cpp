#include "ThreadPoolWrapper.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void ExerciseCpp20Pool(std::atomic<int>& count);

int main()
{
  std::atomic<int> count = 0;
  for (int round = 0; round < 10; ++round) {
    ExerciseCpp20Pool(count);
    ThreadPoolWrapper pool(3);
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) {
      callers.emplace_back([&] {
        for (int j = 0; j < 25; ++j) {
          pool.Push([&count] { ++count; }).get();
        }
      });
    }
    for (auto& caller : callers) {
      caller.join();
    }
    try {
      pool.Push([] { throw std::runtime_error("task failure"); }).get();
      return 2;
    } catch (const std::runtime_error&) {
    }
  }
  if (count != 1500) {
    return 1;
  }
  std::cout << "Mixed C++20/C++23 pools: 1500 tasks, concurrent initialization, "
               "exception delivery and destruction passed\n";
}
