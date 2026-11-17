#pragma once
#include "FormDesc.h"
#include <cstddef>
#include <tuple>

struct Faction
{
  // Snapshot/save resource policy, not an engine limit.
  static constexpr size_t kMaxEntries = 4096;
  FormDesc formDesc;
  int8_t rank = 0;

  bool operator==(const Faction& other) const
  {
    return formDesc == other.formDesc && rank == other.rank;
  }
  bool operator!=(const Faction& other) const { return !(*this == other); }
  bool operator<(const Faction& other) const
  {
    return std::tie(formDesc, rank) < std::tie(other.formDesc, other.rank);
  }
};
