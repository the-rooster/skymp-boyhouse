#include "libespm/SceneActionData.h"
#include "libespm/CTDA.h"
#include "libespm/CombineBrowser.h"
#include "libespm/RecordHeaderAccess.h"
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>

namespace espm {
namespace {
using Fields = std::set<std::string_view>;

template <class T>
T Number(const char* bytes, uint32_t size)
{
  if (size != sizeof(T))
    throw std::runtime_error("Invalid SCEN numeric field size");
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

void Single(Fields& fields, std::string_view tag)
{
  if (!fields.insert(tag).second)
    throw std::runtime_error("Duplicate SCEN field");
}

void Empty(uint32_t size)
{
  if (size)
    throw std::runtime_error("Invalid SCEN marker");
}

uint32_t Form(const LookupResult& record, const char* bytes, uint32_t size)
{
  const auto raw = Number<uint32_t>(bytes, size);
  return raw ? record.ToGlobalId(raw) : 0;
}

void CountCondition(uint32_t& count, uint32_t size)
{
  if (size != sizeof(CTDA))
    throw std::runtime_error("Invalid SCEN condition size");
  ++count;
}

// TES5Edit wbDefinitionsTES5: SNAM is reused after ENAM for a timer.
// The action-local PNAM is a package, distinct from the scene's quest.
void ReadAction(SceneActionData::Action& action, Fields& fields,
                const LookupResult& record, std::string_view tag,
                const char* bytes, uint32_t size)
{
  if (tag == "SNAM" && fields.count("ENAM")) {
    Single(fields, "timer");
    action.timerSeconds = Number<float>(bytes, size);
    return;
  }
  if (tag == "PNAM") {
    action.packageIds.push_back(Form(record, bytes, size));
    return;
  }
  if (tag == "ALID" || tag == "INAM" || tag == "SNAM" || tag == "ENAM" ||
      tag == "FNAM" || tag == "DATA" || tag == "HTID" || tag == "DMIN" ||
      tag == "DMAX" || tag == "DEMO" || tag == "DEVA") {
    Single(fields, tag);
    if (tag == "ALID")
      action.actorAlias = Number<int32_t>(bytes, size);
    else if (tag == "INAM")
      action.index = Number<uint32_t>(bytes, size);
    else if (tag == "SNAM")
      action.startPhase = Number<uint32_t>(bytes, size);
    else if (tag == "ENAM")
      action.endPhase = Number<uint32_t>(bytes, size);
    else if (tag == "FNAM")
      action.flags = Number<uint32_t>(bytes, size);
    else if (tag == "DATA")
      action.topicId = Form(record, bytes, size);
    else if (tag == "HTID")
      action.headtrackActorAlias = Number<int32_t>(bytes, size);
    else if (tag == "DMIN")
      action.loopMin = Number<float>(bytes, size);
    else if (tag == "DMAX")
      action.loopMax = Number<float>(bytes, size);
    else if (tag == "DEMO")
      action.emotionType = Number<uint32_t>(bytes, size);
    else
      action.emotionValue = Number<uint32_t>(bytes, size);
  } else if (tag != "NAM0") {
    action.unknownFields = true;
  }
}

void ReadPhase(SceneActionData::Phase& phase, unsigned& section,
               std::string_view tag, uint32_t size)
{
  if (tag == "NEXT") {
    Empty(size);
    if (++section > 2)
      throw std::runtime_error("Unexpected SCEN phase delimiter");
  } else if (tag == "CTDA" && section < 2) {
    CountCondition(section == 0 ? phase.startConditionCount
                                : phase.completionConditionCount,
                   size);
  } else if (tag == "WNAM") {
    if (section != 2 || size != sizeof(uint32_t))
      throw std::runtime_error("Invalid SCEN editor width");
  } else if (tag != "NAM0" &&
             !((tag == "CIS1" || tag == "CIS2") && section < 2 &&
               (section ? phase.completionConditionCount
                        : phase.startConditionCount))) {
    phase.unknownFields = true;
  }
}
}

SceneActionData SceneActionData::Read(const CombineBrowser& browser,
                                      uint32_t sceneId)
{
  const auto record = browser.LookupById(sceneId);
  if (!record.rec || record.rec->IsDeleted() ||
      record.rec->GetType() != "SCEN")
    throw std::runtime_error("Expected a live winning SCEN");
  SceneActionData scene;
  scene.sceneId = sceneId;
  Phase* phase = nullptr;
  Action* action = nullptr;
  Actor* actor = nullptr;
  unsigned section = 0;
  Fields topFields, actionFields, actorFields;
  std::set<uint32_t> indices, actorIds;
  RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* name, uint32_t size, const char* bytes) {
      const std::string_view tag(name, 4);
      if (tag == "XXXX")
        return;
      if (tag == "ANAM") {
        if (phase || bool(action) != (size == 0))
          throw std::runtime_error("Invalid SCEN action boundary");
        if (action) {
          if (!actionFields.count("INAM") || !actionFields.count("ALID") ||
              !actionFields.count("SNAM") || !actionFields.count("ENAM"))
            throw std::runtime_error("Incomplete SCEN action");
          if (!indices.insert(action->index).second)
            scene.duplicateActionIndices = true;
          action = nullptr;
        } else {
          actor = nullptr;
          actionFields.clear();
          action = &scene.actions.emplace_back();
          action->type = Number<uint16_t>(bytes, size);
        }
      } else if (tag == "HNAM") {
        Empty(size);
        if (action || (phase && section != 2))
          throw std::runtime_error("Invalid SCEN phase boundary");
        actor = nullptr;
        if (phase) {
          phase = nullptr;
        } else {
          phase = &scene.phases.emplace_back();
          section = 0;
        }
      } else if (action) {
        ReadAction(*action, actionFields, record, tag, bytes, size);
      } else if (phase) {
        ReadPhase(*phase, section, tag, size);
      } else if (tag == "ALID") {
        actorFields.clear();
        actor = &scene.actors.emplace_back();
        actor->aliasId = Number<uint32_t>(bytes, size);
        if (!actorIds.insert(actor->aliasId).second)
          throw std::runtime_error("Duplicate SCEN actor");
      } else if ((tag == "LNAM" || tag == "DNAM") && actor) {
        Single(actorFields, tag);
        auto& flags = tag == "LNAM" ? actor->flags : actor->behaviorFlags;
        flags = Number<uint32_t>(bytes, size);
      } else {
        actor = nullptr;
        if (tag == "FNAM" || tag == "PNAM" || tag == "INAM" || tag == "VMAD" ||
            tag == "VNAM") {
          Single(topFields, tag);
          if (tag == "FNAM")
            scene.flags = Number<uint32_t>(bytes, size);
          else if (tag == "PNAM")
            scene.questId = Form(record, bytes, size);
          else if (tag == "VNAM")
            scene.vnam.assign(bytes, bytes + size);
          else if (tag == "VMAD")
            scene.hasScripts = true;
          else
            Number<uint32_t>(bytes, size); // Scene's last action index.
        } else if (tag == "CTDA") {
          CountCondition(scene.conditionCount, size);
        } else if (tag == "NEXT") {
          Empty(size);
        } else if (tag != "EDID" &&
                   !((tag == "CIS1" || tag == "CIS2") &&
                     scene.conditionCount)) {
          scene.unknownFields = true;
        }
      }
    },
    browser.GetCache());
  if (phase || action || !topFields.count("PNAM"))
    throw std::runtime_error("Incomplete SCEN record");
  for (const auto& row : scene.actions) {
    if (row.startPhase > row.endPhase || row.endPhase >= scene.phases.size())
      throw std::runtime_error("Invalid SCEN phase range");
  }
  return scene;
}
}
