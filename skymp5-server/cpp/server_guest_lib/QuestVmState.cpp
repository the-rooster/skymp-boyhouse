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
void BoundedString(const std::string& value, size_t limit,
                   bool allowEmpty = false)
{
  if ((!allowEmpty && value.empty()) || value.size() > limit ||
      value.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid continuation string");
}
void BoundedString(const Json& value, size_t limit, bool allowEmpty = false)
{
  if (!value.is_string())
    throw std::runtime_error("Invalid continuation string");
  BoundedString(value.get_ref<const std::string&>(), limit, allowEmpty);
}
void Fields(const Json& value, std::initializer_list<const char*> names)
{
  if (!value.is_object() || value.size() != names.size())
    throw std::runtime_error("Invalid continuation fields");
  for (const auto* name : names)
    if (!value.contains(name))
      throw std::runtime_error("Missing continuation field");
}
void InstructionCount(const Json& value, bool executed)
{
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() && value.get<uint64_t>() > 100000))
    throw std::runtime_error("Invalid continuation instruction count");
  const auto number = value.get<int64_t>();
  if (number < (executed ? 1 : 0) || number > (executed ? 100000 : 99999))
    throw std::runtime_error("Invalid continuation instruction count");
}
void ValidateDecodedContinuationValue(const Json& saved,
                                      const VarValue& decoded,
                                      const Json& arrays)
{
  const auto type = Type(saved);
  if (decoded.promise || decoded.GetType() != type)
    throw std::runtime_error("Continuation local could not be resolved");
  if (type == VarValue::kType_Object && !saved.at("value").is_null() &&
      !static_cast<IGameObject*>(decoded))
    throw std::runtime_error(
      "Continuation object identity could not be resolved");
  if (!IsArray(type) || saved.at("array").is_null())
    return;
  const auto& elements =
    arrays.at(saved.at("array").get<std::string>()).at("values");
  if (!decoded.pArray || decoded.pArray->size() != elements.size())
    throw std::runtime_error("Continuation array could not be resolved");
  // DecodeObject is supplied by the host. Check its results inside object
  // arrays too, before TakeContinuation publishes a cached allocation or
  // consumes the token. Saved null references remain valid; a nonnull stable
  // identity must not silently become None while restoring suspended work.
  for (size_t i = 0; i < elements.size(); ++i)
    ValidateDecodedContinuationValue(elements[i], (*decoded.pArray)[i],
                                     arrays);
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
    if (!saved.at("schemaVersion").is_number_integer())
      throw std::runtime_error("Invalid saved quest VM version");
    if (saved.at("schemaVersion") != 5 && saved.at("schemaVersion") != 6 &&
        saved.contains("continuations") &&
        (!saved.at("continuations").is_object() ||
         !saved.at("continuations").empty()))
      throw std::runtime_error(
        "Continuations require quest VM version five or six");
    if (saved.at("schemaVersion") == 2) {
      saved["schemaVersion"] = 3;
      if (!saved.contains("globals"))
        saved["globals"] = Json::object();
    }
    if (saved.at("schemaVersion") == 3) {
      saved["schemaVersion"] = 4;
      saved["aliases"] = Json::object();
    }
    if (saved.at("schemaVersion") == 4) {
      saved["schemaVersion"] = 5;
      saved["continuations"] = Json::object();
    }
    if (saved.at("schemaVersion") == 5) {
      if (!saved.at("continuations").is_object() ||
          saved.at("continuations").size() > 256)
        throw std::runtime_error("Invalid saved quest continuations");
      for (auto& value : saved["continuations"]) {
        // Only the old single-frame shape is migratable. In particular, never
        // erase invented caller data or an unrecognized future node version.
        Fields(value,
               { "schemaVersion", "scope", "receiver", "rootScript", "script",
                 "state", "function", "contentHashes", "resultType",
                 "instruction", "executedInstructions", "locals" });
        if (!value.at("schemaVersion").is_number_integer() ||
            value.at("schemaVersion") != 1)
          throw std::runtime_error(
            "Unsupported legacy quest continuation version");
        value["schemaVersion"] = 2;
        value["kind"] = "method";
        value["callers"] = Json::array();
      }
      saved["schemaVersion"] = 6;
    } else if (saved.at("schemaVersion") != 6) {
      throw std::runtime_error("Unsupported saved quest VM version");
    }
    Validate(saved);
    return saved;
  }
  // Legacy scripts stored inline, non-shared arrays. Assign a distinct heap
  // identity to each of those allocations; never deduplicate by array
  // contents.
  Json result = {
    { "schemaVersion", 6 },        { "scripts", saved },
    { "arrays", Json::object() },  { "globals", Json::object() },
    { "aliases", Json::object() }, { "continuations", Json::object() }
  };
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
  if (!saved.at("schemaVersion").is_number_integer() ||
      saved.at("schemaVersion") != 6 || !saved.at("scripts").is_object() ||
      !saved.at("arrays").is_object() || saved["arrays"].size() > 100000)
    throw std::runtime_error("Invalid saved quest heap");
  {
    if (!saved.at("globals").is_object())
      throw std::runtime_error("Invalid saved campaign globals");
    for (const auto& [key, value] : saved["globals"].items())
      if (key.empty() || key.size() > 300 || !value.is_number() ||
          !std::isfinite(value.get<double>()) ||
          !std::isfinite(static_cast<float>(value.get<double>())))
        throw std::runtime_error("Invalid saved campaign global");
  }
  if (!saved.at("aliases").is_object() || saved["aliases"].size() > 100000)
    throw std::runtime_error("Invalid campaign alias bindings");
  for (const auto& [key, value] : saved["aliases"].items())
    if (key.empty() || key.size() > 500 ||
        (!value.is_null() &&
         (!value.is_string() || value.get_ref<const std::string&>().empty() ||
          value.get_ref<const std::string&>().size() > 300)))
      throw std::runtime_error("Invalid campaign alias binding");
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
    for (const auto& value : script["values"])
      ValidateValue(value, arrays);
  }
  if (!saved.at("continuations").is_object() ||
      saved.at("continuations").size() > 256)
    throw std::runtime_error("Invalid saved quest continuations");
  for (const auto& [token, continuation] : saved["continuations"].items())
    ValidateContinuation(token, continuation, arrays);
}

void QuestVmState::ValidateValue(const Json& value, const Json& arrays)
{
  const auto type = Type(value);
  if (!IsArray(type)) {
    ValidateScalar(value);
    return;
  }
  const auto& ref = value.at("array");
  if (ref.is_null())
    return;
  if (!ref.is_string() || !arrays.contains(ref.get<std::string>()) ||
      arrays.at(ref.get<std::string>()).at("type") != type)
    throw std::runtime_error("Dangling or mistyped quest array reference");
}

void QuestVmState::ValidateContinuation(const std::string& token,
                                       const Json& value, const Json& arrays)
{
  BoundedString(token, 256);
  Fields(value,
         { "schemaVersion", "scope", "receiver", "kind", "rootScript",
           "script", "state", "function", "contentHashes", "resultType",
           "instruction", "executedInstructions", "locals", "callers" });
  if (!value.at("schemaVersion").is_number_integer() ||
      value.at("schemaVersion") != 2)
    throw std::runtime_error("Unsupported quest continuation version");
  BoundedString(value.at("scope"), 300);
  InstructionCount(value.at("executedInstructions"), true);
  ValidateFrame(value, arrays);
  const auto& callers = value.at("callers");
  if (!callers.is_array() || callers.size() > 127)
    throw std::runtime_error("Invalid continuation caller stack");
  size_t totalLocals = value.at("locals").size();
  for (const auto& caller : callers) {
    // Caller frames share the node's schema version. They have no operation
    // token, scope, budget, or nested caller list of their own.
    Fields(caller,
           { "receiver", "kind", "rootScript", "script", "state", "function",
             "contentHashes", "resultType", "instruction", "locals" });
    ValidateFrame(caller, arrays);
    if (caller.at("locals").size() > 65535 - totalLocals)
      throw std::runtime_error("Continuation stack local limit exceeded");
    totalLocals += caller.at("locals").size();
  }
}

void QuestVmState::ValidateFrame(const Json& value, const Json& arrays)
{
  const auto& kind = value.at("kind");
  if (kind != "method" && kind != "static" && kind != "propertyGet" &&
      kind != "propertySet")
    throw std::runtime_error("Unsupported continuation frame kind");
  for (const auto* field : { "rootScript", "script", "function", "resultType" })
    BoundedString(value.at(field), 1024);
  BoundedString(value.at("state"), 1024, true);
  InstructionCount(value.at("instruction"), false);
  const auto& receiver = value.at("receiver");
  if (Type(receiver) != VarValue::kType_Object)
    throw std::runtime_error("Continuation frame receiver must be an object");
  ValidateScalar(receiver);
  if (kind == "static") {
    if (!receiver.at("value").is_null() || value.at("state") != "" ||
        value.at("rootScript") != value.at("script"))
      throw std::runtime_error("Invalid static continuation frame");
  } else if (receiver.at("value").is_null()) {
    throw std::runtime_error("Continuation frame requires a nonnull receiver");
  }
  const auto& hashes = value.at("contentHashes");
  if (!hashes.is_array() || hashes.size() > 128)
    throw std::runtime_error("Invalid continuation content hashes");
  std::set<std::string> scripts;
  for (const auto& hash : hashes) {
    Fields(hash, { "script", "hash" });
    BoundedString(hash.at("script"), 1024);
    if (!scripts.insert(hash.at("script").get<std::string>()).second)
      throw std::runtime_error("Duplicate continuation content hash");
    BoundedString(hash.at("hash"), 64);
    const auto& digest = hash.at("hash").get_ref<const std::string&>();
    if (digest.size() != 64 ||
        digest.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      throw std::runtime_error("Invalid continuation content digest");
  }
  const auto& locals = value.at("locals");
  if (!locals.is_array() || locals.size() > 65535)
    throw std::runtime_error("Invalid continuation locals");
  std::set<std::string> names;
  for (const auto& local : locals) {
    Fields(local, { "name", "declaredType", "value" });
    BoundedString(local.at("name"), 1024);
    BoundedString(local.at("declaredType"), 1024);
    if (!names.insert(local.at("name").get<std::string>()).second)
      throw std::runtime_error("Duplicate continuation local");
    ValidateValue(local.at("value"), arrays);
  }
}

void QuestVmState::Collect(Json& saved)
{
  std::set<std::string> reachable;
  for (const auto& script : saved["scripts"])
    for (const auto& value : script["values"])
      if (IsArray(Type(value)) && !value.at("array").is_null())
        reachable.insert(value.at("array").get<std::string>());
  auto collectFrame = [&](const Json& frame) {
    for (const auto& local : frame["locals"]) {
      const auto& value = local.at("value");
      if (IsArray(Type(value)) && !value.at("array").is_null())
        reachable.insert(value.at("array").get<std::string>());
    }
  };
  for (const auto& continuation : saved["continuations"]) {
    collectFrame(continuation);
    for (const auto& caller : continuation["callers"])
      collectFrame(caller);
  }
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
  if (saved.is_object() && saved.empty())
    return;
  auto updated = Normalize(saved);
  // A suspended frame may refer to this quest through locals, aliases, or
  // another script. Scope alone is not a complete dependency/cancellation map.
  if (!updated["continuations"].empty())
    throw std::runtime_error("Cannot reset quests with pending continuations");
  auto& scripts = updated["scripts"];
  for (auto it = scripts.begin(); it != scripts.end();)
    if (it.key().compare(0, prefix.size(), prefix) == 0)
      it = scripts.erase(it);
    else
      ++it;
  auto& aliases = updated["aliases"];
  for (auto it = aliases.begin(); it != aliases.end();)
    if (it.key().compare(0, prefix.size(), prefix) == 0)
      it = aliases.erase(it);
    else
      ++it;
  Collect(updated);
  if (scripts.empty() && aliases.empty() &&
      updated.value("globals", Json::object()).empty())
    updated = Json::object();
  saved = std::move(updated);
}

void QuestVmState::PutContinuation(const PapyrusContinuation& continuation)
{
  BoundedString(continuation.token, 256);
  if (state["continuations"].contains(continuation.token))
    throw std::runtime_error("Duplicate quest continuation token");
  if (state["continuations"].size() >= 256 ||
      continuation.locals.size() > 65535 ||
      continuation.contentHashes.size() > 128 ||
      continuation.callers.size() > 127)
    throw std::runtime_error("Quest continuation limit exceeded");
  size_t totalLocals = continuation.locals.size();
  for (const auto& caller : continuation.callers) {
    if (caller.locals.size() > 65535 - totalLocals ||
        caller.contentHashes.size() > 128)
      throw std::runtime_error("Quest continuation stack limit exceeded");
    totalLocals += caller.locals.size();
  }

  // Encoding can allocate heap identities, and saving checks all live arrays.
  // Publish those changes only after the entire candidate has been validated.
  auto updated = *this;
  auto value = updated.EncodeFrame(continuation);
  value["schemaVersion"] = 2;
  value["scope"] = continuation.scope;
  value["executedInstructions"] = continuation.executedInstructions;
  value["callers"] = Json::array();
  for (const auto& caller : continuation.callers)
    value["callers"].push_back(updated.EncodeFrame(caller));
  updated.state["continuations"][continuation.token] = std::move(value);
  updated.Save();
  *this = std::move(updated);
}

QuestVmState::Json QuestVmState::EncodeFrame(const PapyrusFrame& frame)
{
  if (frame.receiver.GetType() != VarValue::kType_Object ||
      (frame.kind == "static") !=
        (static_cast<IGameObject*>(frame.receiver) == nullptr))
    throw std::runtime_error("Invalid continuation frame receiver");
  Json value = { { "receiver", Encode(frame.receiver) },
                 { "kind", frame.kind },
                 { "rootScript", frame.rootScript },
                 { "script", frame.script },
                 { "state", frame.state },
                 { "function", frame.function },
                 { "contentHashes", Json::array() },
                 { "resultType", frame.resultType },
                 { "instruction", frame.instruction },
                 { "locals", Json::array() } };
  for (const auto& [script, hash] : frame.contentHashes)
    value["contentHashes"].push_back({ { "script", script }, { "hash", hash } });
  for (const auto& [name, local] : frame.locals)
    value["locals"].push_back({ { "name", name },
                                { "declaredType", local.objectType },
                                { "value", Encode(local) } });
  return value;
}

PapyrusContinuation QuestVmState::TakeContinuation(const std::string& scope,
                                                  const std::string& token)
{
  BoundedString(scope, 300);
  BoundedString(token, 256);
  auto updated = *this;
  updated.Save();
  const auto& pending = updated.state["continuations"];
  if (!pending.contains(token) || pending.at(token).at("scope") != scope)
    throw std::runtime_error("Quest continuation scope or token mismatch");
  const auto& value = pending.at(token);
  PapyrusContinuation result;
  static_cast<PapyrusFrame&>(result) = updated.DecodeFrame(value);
  result.scope = scope;
  result.token = token;
  result.executedInstructions =
    value.at("executedInstructions").get<uint32_t>();
  for (const auto& caller : value.at("callers"))
    result.callers.push_back(updated.DecodeFrame(caller));
  updated.state["continuations"].erase(token);
  Collect(updated.state);
  *this = std::move(updated);
  return result;
}

PapyrusFrame QuestVmState::DecodeFrame(const Json& value)
{
  PapyrusFrame result;
  result.kind = value.at("kind").get<std::string>();
  result.receiver = Decode(value.at("receiver"));
  if (result.receiver.promise ||
      result.receiver.GetType() != VarValue::kType_Object ||
      (result.kind == "static") !=
        (static_cast<IGameObject*>(result.receiver) == nullptr))
    throw std::runtime_error("Continuation receiver could not be resolved");
  result.rootScript = value.at("rootScript").get<std::string>();
  result.script = value.at("script").get<std::string>();
  result.state = value.at("state").get<std::string>();
  result.function = value.at("function").get<std::string>();
  result.resultType = value.at("resultType").get<std::string>();
  result.instruction = value.at("instruction").get<uint32_t>();
  for (const auto& hash : value.at("contentHashes"))
    result.contentHashes.emplace_back(hash.at("script").get<std::string>(),
                                      hash.at("hash").get<std::string>());
  for (const auto& local : value.at("locals")) {
    auto decoded = Decode(local.at("value"));
    ValidateDecodedContinuationValue(local.at("value"), decoded,
                                     state.at("arrays"));
    decoded.objectType = local.at("declaredType").get<std::string>();
    result.locals.emplace_back(local.at("name").get<std::string>(), decoded);
  }
  return result;
}

std::optional<float> QuestVmState::GetGlobal(const std::string& key) const
{
  if (!state.contains("globals") || !state["globals"].contains(key))
    return std::nullopt;
  return state["globals"][key].get<float>();
}

void QuestVmState::SetGlobal(const std::string& key, double value)
{
  if (key.empty() || key.size() > 300 || !std::isfinite(value) ||
      !std::isfinite(static_cast<float>(value)))
    throw std::runtime_error("Invalid campaign global value");
  // FLTV and the Skyrim native API use 32-bit float storage.
  state["globals"][key] = static_cast<float>(value);
}

std::optional<QuestVmState::Json> QuestVmState::GetAliasBinding(
  const std::string& key) const
{
  if (!state["aliases"].contains(key))
    return std::nullopt;
  return std::optional<Json>{ std::in_place, state["aliases"][key] };
}
void QuestVmState::SetAliasBinding(const std::string& key, Json value)
{
  if (key.empty() || key.size() > 500 ||
      (!value.is_null() &&
       (!value.is_string() || value.get_ref<const std::string&>().empty() ||
        value.get_ref<const std::string&>().size() > 300)))
    throw std::runtime_error("Invalid campaign alias binding");
  state["aliases"][key] = std::move(value);
}
