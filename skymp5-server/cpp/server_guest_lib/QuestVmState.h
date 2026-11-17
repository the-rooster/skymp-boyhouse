#pragma once
#include "papyrus-vm/Continuation.h"
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>

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
  std::optional<float> GetGlobal(const std::string& key) const;
  void SetGlobal(const std::string& key, double value);
  std::optional<Json> GetAliasBinding(const std::string& key) const;
  void SetAliasBinding(const std::string& key, Json value);
  // Capture a stack using this campaign's stable object/array identities.
  // A completion consumes its token once; the caller must atomically commit
  // consumption together with resumed effects and durable event delivery.
  void PutContinuation(const PapyrusContinuation& continuation);
  PapyrusContinuation TakeContinuation(const std::string& scope,
                                       const std::string& token);
  static void ResetQuest(Json& saved, const std::string& prefix);

private:
  static Json Normalize(Json saved);
  static void Validate(const Json& saved);
  static void Collect(Json& saved);
  static void ValidateScalar(const Json& value);
  static void ValidateValue(const Json& value, const Json& arrays);
  static void ValidateFrame(const Json& value, const Json& arrays);
  static void ValidateContinuation(const std::string& token, const Json& value,
                                   const Json& arrays);
  Json EncodeFrame(const PapyrusFrame& frame);
  PapyrusFrame DecodeFrame(const Json& frame);
  Json state;
  EncodeObject encodeObject;
  DecodeObject decodeObject;
  uint64_t nextId = 1;
  std::map<const void*, std::string> identities;
  std::map<std::string, VarValue> loaded;
};
