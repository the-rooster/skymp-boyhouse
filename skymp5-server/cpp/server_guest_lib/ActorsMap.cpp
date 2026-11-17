#include "ActorsMap.h"
#include "Config.h"
#include "MpActor.h"
#include <sstream>
#include <stdexcept>

ActorsMap::ActorsMap()
{
  actorByUserId.resize(kMaxPlayers, nullptr);
}

void ActorsMap::Set(Networking::UserId userId, MpActor* actor)
{
  if (userId == Networking::InvalidUserId) {
    throw std::runtime_error(
      "Trying to insert Networking::InvalidUserId into ActorsMap");
  }
  if (!actor) {
    throw std::runtime_error("Trying to insert nullptr into ActorsMap");
  }
  if (userId >= actorByUserId.size()) {
    std::stringstream ss;
    ss << "UserId " << userId << " is too big to be stored in ActorsMap";
    throw std::runtime_error(ss.str());
  }
  // Allocate the reverse entry before disturbing either existing assignment.
  auto [entry, inserted] = userIdByActor.try_emplace(actor, userId);
  if (!inserted)
    actorByUserId[entry->second] = nullptr;
  if (const auto previous = actorByUserId[userId];
      previous && previous != actor)
    userIdByActor.erase(previous);
  entry->second = userId;
  actorByUserId[userId] = actor;
}

void ActorsMap::Erase(Networking::UserId userId)
{
  auto actor = Find(userId);
  if (actor) {
    userIdByActor.erase(actor);
    actorByUserId[userId] = nullptr;
  }
}

void ActorsMap::Erase(MpActor* actor)
{
  auto userId = Find(actor);
  if (userId != Networking::InvalidUserId) {
    userIdByActor.erase(actor);
    actorByUserId[userId] = nullptr;
  }
}

// Returns Networking::InvalidUserId if no userId found
Networking::UserId ActorsMap::Find(MpActor* actor)
{
  auto it = userIdByActor.find(actor);
  if (it == userIdByActor.end()) {
    return Networking::InvalidUserId;
  }
  return it->second;
}

// Returns nullptr if no actor found
MpActor* ActorsMap::Find(Networking::UserId userId)
{
  if (userId >= actorByUserId.size()) {
    return nullptr;
  }
  return actorByUserId[userId];
}
