#pragma once
#include <cstdint>
#include <functional>
#include <future>
#include <memory>

class ThreadPoolWrapper
{
public:
  ThreadPoolWrapper(int32_t numThreads = 1);
  ~ThreadPoolWrapper();

  std::future<void> Push(const std::function<void()>& task);

  void PushAndWait(const std::function<void()>& task) { Push(task).wait(); }

private:
  // BS_thread_pool changes its layout with C++23's move_only_function. Keep
  // its definition out of this header: CommonLib consumers use C++23 while
  // platform_lib uses C++20, so inline pool code violated the ODR and crashed.
  struct Impl;
  std::unique_ptr<Impl> pImpl;
};
