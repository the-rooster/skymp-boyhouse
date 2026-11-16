#include "QuestVmState.h"
#include <charconv>
#include <cmath>
#include <set>

namespace {
using Json = nlohmann::json;
bool IsArray(int64_t type)
{
  return type >= 11 && type <= 15;
}
int ElementType(int type)
{
  return type == 11 ? 0 : type - 10;
}
int Type(const Json& value)
{
  if (!value.is_object() || !value.at("type").is_number_integer())
    throw std::runtime_error("Invalid saved Papyrus type");
  if (value.at("type").is_number_unsigned() &&
      value.at("type").get<uint64_t>() > 15)
    throw std::runtime_error("Unsupported saved Papyrus type");
  const auto type = value.at("type").get<int64_t>();
  if (type != 0 && (type < 2 || type > 5) && !IsArray(type))
    throw std::runtime_error("Unsupported saved Papyrus type");
  return int(type);
}
uint64_t HeapId(const std::string& text)
{
  uint64_t id = 0;
  const auto parsed =
    std::from_chars(text.data(), text.data() + text.size(), id);
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
      !id || id == UINT64_MAX || std::to_string(id) != text)
    throw std::runtime_error("Invalid saved Papyrus array identity");
  return id;
}
}

void QuestVmState::ValidateScalar(const Json& value)
{
  const auto type = Type(value);
  const auto& data = value.at("value");
  switch (type) {
    case 0:
      // Object IDs are interpreted by the campaign's object resolver.
      if (!data.is_null() && !data.is_string() && !data.is_object())
        throw std::runtime_error("Invalid saved Papyrus object");
      return;
    case 2:
      if (data.is_string())
        return;
      break;
    case 3:
      if (data.is_number_unsigned()) {
        if (data.get<uint64_t>() <= INT32_MAX)
          return;
      } else if (data.is_number_integer() &&
                 data.get<int64_t>() >= INT32_MIN &&
                 data.get<int64_t>() <= INT32_MAX)
        return;
      break;
    case 4:
      if (data.is_number() && std::isfinite(data.get<double>()))
        return;
      break;
    case 5:
      if (data.is_boolean())
        return;
      break;
  }
  throw std::runtime_error("Invalid saved Papyrus scalar");
}

QuestVmState::Json QuestVmState::Normalize(Json saved)
{
  if (!saved.is_object())
    throw std::runtime_error("Invalid saved quest VM");
  if (saved.contains("schemaVersion")) {
    if (saved.at("schemaVersion") != 2)
      throw std::runtime_error("Unsupported saved quest VM version");
    Validate(saved);
    return saved;
  }
  // Legacy scripts stored inline, non-shared arrays. Assign a distinct heap
  // identity to each of those allocations; never deduplicate by array
  // contents.
  Json result = { { "schemaVersion", 2 },
                  { "scripts", saved },
                  { "arrays", Json::object() } };
  uint64_t id = 1;
  for (auto& [key, script] : result["scripts"].items()) {
    if (!script.is_object() || script.at("schemaVersion") != 1 ||
        !script.at("values").is_object())
      throw std::runtime_error("Invalid legacy quest script");
    for (auto& value : script["values"]) {
      const auto type = Type(value);
      if (!IsArray(type)) {
        ValidateScalar(value);
        continue;
      }
      const auto old = value.at("value");
      if (old.is_null()) {
        value = { { "type", type }, { "array", nullptr } };
        continue;
      }
      if (!old.is_array() || old.size() > 1000000)
        throw std::runtime_error("Invalid legacy quest array");
      const auto identity = std::to_string(id++);
      result["arrays"][identity] = { { "type", type }, { "values", old } };
      value = { { "type", type }, { "array", identity } };
    }
  }
  Validate(result);
  return result;
}

void QuestVmState::Validate(const Json& saved)
{
  if (saved.at("schemaVersion") != 2 || !saved.at("scripts").is_object() ||
      !saved.at("arrays").is_object() || saved["arrays"].size() > 100000)
    throw std::runtime_error("Invalid saved quest heap");
  const auto& arrays = saved["arrays"];
  for (const auto& [id, node] : arrays.items()) {
    HeapId(id);
    const auto type = Type(node);
    if (!IsArray(type) || !node.at("values").is_array() ||
        node["values"].size() > 1000000)
      throw std::runtime_error("Invalid saved Papyrus array");
    for (const auto& entry : node["values"]) {
      if (Type(entry) != ElementType(type))
        throw std::runtime_error("Saved array element type mismatch");
      ValidateScalar(entry);
    }
  }
  for (const auto& [key, script] : saved["scripts"].items()) {
    if (key.empty() || key.size() > 1024 || !script.is_object() ||
        script.at("schemaVersion") != 1 || !script.at("values").is_object())
      throw std::runtime_error("Invalid saved quest script");
    for (const auto& value : script["values"]) {
      const auto type = Type(value);
      if (!IsArray(type)) {
        ValidateScalar(value);
        continue;
      }
      const auto& ref = value.at("array");
      if (ref.is_null())
        continue;
      if (!ref.is_string() || !arrays.contains(ref.get<std::string>()) ||
          arrays.at(ref.get<std::string>()).at("type") != type)
        throw std::runtime_error("Dangling or mistyped quest array reference");
    }
  }
}

void QuestVmState::Collect(Json& saved)
{
  std::set<std::string> reachable;
  for (const auto& script : saved["scripts"])
    for (const auto& value : script["values"])
      if (IsArray(Type(value)) && !value.at("array").is_null())
        reachable.insert(value.at("array").get<std::string>());
  auto& arrays = saved["arrays"];
  for (auto it = arrays.begin(); it != arrays.end();)
    if (!reachable.count(it.key()))
      it = arrays.erase(it);
    else
      ++it;
}

QuestVmState::QuestVmState(Json saved, EncodeObject encode,
                           DecodeObject decode)
  : state(Normalize(std::move(saved)))
  , encodeObject(std::move(encode))
  , decodeObject(std::move(decode))
{
  for (const auto& [id, node] : state["arrays"].items())
    nextId = std::max(nextId, HeapId(id) + 1);
}

QuestVmState::Json QuestVmState::GetScript(const std::string& key) const
{
  return state["scripts"].value(key, Json::object());
}

void QuestVmState::SetScript(const std::string& key, Json script)
{
  state["scripts"][key] = std::move(script);
}

QuestVmState::Json QuestVmState::Encode(const VarValue& value)
{
  if (value.promise)
    throw std::runtime_error("Cannot save latent quest value");
  const auto type = value.GetType();
  Json result = { { "type", type } };
  switch (type) {
    case 0: {
      auto* object = static_cast<IGameObject*>(value);
      result["value"] = object ? encodeObject(object) : Json(nullptr);
      break;
    }
    case 2:
      result["value"] = std::string(static_cast<const char*>(value));
      break;
    case 3:
      result["value"] = static_cast<int32_t>(value);
      break;
    case 4:
      result["value"] = static_cast<double>(value);
      break;
    case 5:
      result["value"] = static_cast<bool>(value);
      break;
    default: {
      if (!IsArray(type))
        throw std::runtime_error("Unsupported quest variable type");
      result["array"] = nullptr;
      if (value.pArray) {
        auto it = identities.find(value.pArray.get());
        if (it == identities.end()) {
          if (nextId == UINT64_MAX)
            throw std::runtime_error("Quest heap identity exhausted");
          const auto id = std::to_string(nextId++);
          it = identities.emplace(value.pArray.get(), id).first;
          loaded.emplace(id,
                         value); // Pin allocations until the transaction ends.
        }
        if (loaded.at(it->second).GetType() != type)
          throw std::runtime_error("Conflicting quest array types");
        result["array"] = it->second;
      }
      return result;
    }
  }
  ValidateScalar(result);
  return result;
}

VarValue QuestVmState::Decode(const Json& value)
{
  const auto type = Type(value);
  if (IsArray(type)) {
    VarValue result(static_cast<uint8_t>(type));
    const auto& ref = value.at("array");
    if (ref.is_null())
      return result;
    const auto id = ref.get<std::string>();
    if (loaded.count(id)) {
      if (loaded.at(id).GetType() != type)
        throw std::runtime_error("Conflicting quest array types");
      return loaded.at(id);
    }
    const auto& node = state["arrays"].at(id);
    if (node.at("type") != type)
      throw std::runtime_error("Conflicting quest array types");
    result.pArray = std::make_shared<std::vector<VarValue>>();
    for (const auto& entry : node.at("values"))
      result.pArray->push_back(Decode(entry));
    identities.emplace(result.pArray.get(), id);
    loaded.emplace(id, result);
    return result;
  }
  ValidateScalar(value);
  const auto& data = value.at("value");
  switch (type) {
    case 0:
      return data.is_null() ? VarValue::None() : decodeObject(data);
    case 2:
      return VarValue(data.get<std::string>());
    case 3:
      return VarValue(data.get<int32_t>());
    case 4:
      return VarValue(data.get<double>());
    case 5:
      return VarValue(data.get<bool>());
  }
  throw std::runtime_error("Invalid saved quest value");
}

QuestVmState::Json QuestVmState::Save()
{
  for (const auto& [id, array] : loaded) {
    Json values = Json::array();
    if (array.pArray->size() > 1000000)
      throw std::runtime_error("Quest array too large");
    for (const auto& value : *array.pArray) {
      if (value.GetType() != ElementType(array.GetType()))
        throw std::runtime_error("Quest array element type mismatch");
      values.push_back(Encode(value));
    }
    state["arrays"][id] = { { "type", array.GetType() },
                            { "values", values } };
  }
  Validate(state);
  Collect(state);
  return state;
}

void QuestVmState::ResetQuest(Json& saved, const std::string& prefix)
{
  if (saved.empty())
    return;
  saved = Normalize(std::move(saved));
  auto& scripts = saved["scripts"];
  for (auto it = scripts.begin(); it != scripts.end();)
    if (it.key().compare(0, prefix.size(), prefix) == 0)
      it = scripts.erase(it);
    else
      ++it;
  Collect(saved);
  if (scripts.empty())
    saved = Json::object();
}
