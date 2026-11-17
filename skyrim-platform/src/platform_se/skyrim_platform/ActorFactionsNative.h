#pragma once
#include "ActorFactionsPlan.h"

namespace ActorFactionsNative {
struct Identity
{
  uint32_t actorId = 0, baseId = 0;
  RE::ActorHandle handle;
};
struct Snapshot
{
  Identity identity;
  ActorFactions::Base base;
  ActorFactions::Rows rows;
  ActorFactions::Target observed;
  bool hadExtra = false, player = false, unique = false;
  uint64_t update = 0;
};
bool SupportedRuntime();
Identity Bind(uint32_t actorId);
// nullptr means the original handle no longer names a live actor. It never
// authorizes a replacement actor which happens to reuse the same FormID.
RE::NiPointer<RE::Actor> Resolve(const Identity& identity);
Snapshot Read(const Identity&, const ActorFactions::Target& candidates = {});
void ValidateSnapshot(const Snapshot&);
// All preflight/allocation precedes the first publication. The callback arms
// the persistent native uncertainty barrier immediately before mutation.
void Write(const Snapshot& before, const ActorFactions::Rows& rows,
           bool requireExtra, const std::function<void()>& arm);
}
