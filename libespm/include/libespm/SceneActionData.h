#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace espm {
class CombineBrowser;

// Read-only scene metadata from the winning SCEN. Form IDs are global,
// while actor IDs are indices in the owning quest's alias table.
struct SceneActionData
{
  struct Phase
  {
    uint32_t startConditionCount = 0, completionConditionCount = 0;
    bool unknownFields = false;
  };
  struct Actor
  {
    uint32_t aliasId = 0, flags = 0, behaviorFlags = 0;
  };
  struct Action
  {
    uint16_t type = 0; // 0 dialogue, 1 package, 2 timer; retain other values.
    uint32_t index = 0, startPhase = 0, endPhase = 0, flags = 0, topicId = 0;
    int32_t actorAlias = -1, headtrackActorAlias = -1;
    std::optional<float> loopMin, loopMax, timerSeconds;
    std::optional<uint32_t> emotionType, emotionValue;
    std::vector<uint32_t> packageIds;
    bool unknownFields = false;
  };

  uint32_t sceneId = 0, questId = 0, flags = 0, conditionCount = 0;
  bool hasScripts = false, unknownFields = false,
       duplicateActionIndices = false;
  // xEdit does not assign semantics to the scene-level VNAM.
  std::vector<uint8_t> vnam;
  std::vector<Phase> phases;
  std::vector<Actor> actors;
  std::vector<Action> actions;

  static SceneActionData Read(const CombineBrowser& browser, uint32_t sceneId);
};
}
