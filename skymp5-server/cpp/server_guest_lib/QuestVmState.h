#pragma once
#include "papyrus-vm/VarValue.h"
#include <functional>
#include <map>
#include <nlohmann/json.hpp>

// One heap per campaign transaction. Persist array identities across scripts;
// resolving game objects stays with the campaign-specific runtime.
class QuestVmState
{
public:
  using Json = nlohmann::json;
  using EncodeObject = std::function<Json(IGameObject*)>;
  using DecodeObject = std::function<VarValue(const Json&)>;
  QuestVmState(Json saved, EncodeObject encodeObject,
               DecodeObject decodeObject);
  Json GetScript(const std::string& key) const;
  void SetScript(const std::string& key, Json script);
  Json Encode(const VarValue& value);
  VarValue Decode(const Json& value);
  Json Save();
  static void ResetQuest(Json& saved, const std::string& prefix);

private:
  static Json Normalize(Json saved);
  static void Validate(const Json& saved);
  static void Collect(Json& saved);
  static void ValidateScalar(const Json& value);
  Json state;
  EncodeObject encodeObject;
  DecodeObject decodeObject;
  uint64_t nextId = 1;
  std::map<const void*, std::string> identities;
  std::map<std::string, VarValue> loaded;
};
