#include "ThreadPoolWrapper.h"
#include <atomic>

void ExerciseCpp20Pool(std::atomic<int>& count)
{
  ThreadPoolWrapper pool(3);
  for (int i = 0; i < 50; ++i) {
    pool.PushAndWait([&count] { ++count; });
  }
}
