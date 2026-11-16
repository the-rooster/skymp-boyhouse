#pragma once
#include "PlayerLoadoutPlan.h"

namespace PlayerLoadoutNative {
struct Binding
{
  RE::ExtraDataList* list = nullptr;
};
// Borrowed pointers are valid only until the first engine mutation. Never
// store this object in a request or observation barrier; store .value only.
struct Snapshot
{
  PlayerLoadout::Snapshot value;
  std::map<uint64_t, Binding> bindings;
};
bool Ready();
bool Readable();
Snapshot Read(const PlayerLoadout::Target& target,
              const std::array<uint32_t, 4>& observedSpells);
void Preflight(const Snapshot& snapshot, const PlayerLoadout::Target& target,
               const PlayerLoadout::Plan& plan);
void Apply(const Snapshot& snapshot,
           const PlayerLoadout::Operation& operation);
}
