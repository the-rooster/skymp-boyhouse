#include "papyrus-vm/VirtualMachine.h"
#include <catch2/catch_all.hpp>
#include <limits>

TEST_CASE("Quest jobs restore cumulative VM work without replenishing quota",
          "[Quest][QuestJobBudget]")
{
  VirtualMachine first(std::vector<PexScript::Lazy>{});
  first.SetStrictExecution(true);
  first.RestoreExecutionBudget(99998);
  first.CheckExecutionBudget();
  REQUIRE(first.GetExecutedInstructions() == 99999);
  // A checkpoint's lower count cannot erase work already done this segment.
  first.RestoreExecutionBudget(5);
  REQUIRE(first.GetExecutedInstructions() == 99999);

  VirtualMachine resumed(std::vector<PexScript::Lazy>{});
  resumed.SetStrictExecution(true);
  resumed.RestoreExecutionBudget(first.GetExecutedInstructions());
  REQUIRE_NOTHROW(resumed.CheckExecutionBudget());
  REQUIRE(resumed.GetExecutedInstructions() == 100000);
  REQUIRE_THROWS(resumed.CheckExecutionBudget());
  const auto exhausted = resumed.GetExecutedInstructions();
  resumed.RestoreExecutionBudget(0);
  resumed.RestoreExecutionBudget(100000);
  REQUIRE(resumed.GetExecutedInstructions() == exhausted);
  REQUIRE_THROWS(resumed.CheckExecutionBudget());
}

TEST_CASE("Invalid job budget restoration leaves VM accounting intact",
          "[Quest][QuestJobBudget]")
{
  VirtualMachine vm(std::vector<PexScript::Lazy>{});
  REQUIRE_THROWS(vm.RestoreExecutionBudget(0));
  REQUIRE(vm.GetExecutedInstructions() == 0);
  vm.SetStrictExecution(true);
  vm.RestoreExecutionBudget(7);
  REQUIRE_THROWS(vm.RestoreExecutionBudget(100001));
  REQUIRE_THROWS(vm.RestoreExecutionBudget(
    std::numeric_limits<uint32_t>::max()));
  REQUIRE(vm.GetExecutedInstructions() == 7);
  vm.RestoreExecutionBudget(7);
  vm.CheckExecutionBudget();
  REQUIRE(vm.GetExecutedInstructions() == 8);
}
