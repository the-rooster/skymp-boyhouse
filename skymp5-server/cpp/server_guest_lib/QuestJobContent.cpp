#include "QuestJobContent.h"
#include "FormDesc.h"
#include "WorldState.h"
#include "libespm/Loader.h"
#include "libespm/RecordHeaderAccess.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sodium.h>
#include <stdexcept>
#include <string_view>

namespace {
std::string Name(std::string value)
{
  if (value.empty() || value.size() > 1024 ||
      value.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid quest content script name");
  for (auto& byte : value)
    if (byte >= 'A' && byte <= 'Z')
      byte += 'a' - 'A';
  return value;
}

std::string Digest(std::string value)
{
  if (value.size() != 64 ||
      value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw std::runtime_error("Invalid quest content digest");
  for (auto& byte : value)
    if (byte >= 'A' && byte <= 'F')
      byte += 'a' - 'A';
  return value;
}

class Hasher
{
public:
  explicit Hasher(std::string_view domain)
  {
    if (crypto_hash_sha256_init(&state) != 0)
      throw std::runtime_error("Cannot initialize quest content hash");
    Text(domain);
  }
  void Bytes(const void* data, size_t count)
  {
    constexpr size_t limit = 64 * 1024 * 1024;
    if (count > limit - bytes)
      throw std::runtime_error("Quest content encoding exceeds limit");
    bytes += count;
    if (count && crypto_hash_sha256_update(
                   &state, static_cast<const unsigned char*>(data), count) != 0)
      throw std::runtime_error("Cannot update quest content hash");
  }
  void Number(uint64_t value, size_t width)
  {
    std::array<unsigned char, 8> encoded{};
    for (size_t i = 0; i < width; ++i) {
      encoded[width - i - 1] = static_cast<unsigned char>(value & 255);
      value >>= 8;
    }
    Bytes(encoded.data(), width);
  }
  void Count(size_t value, size_t limit = 65535)
  {
    if (value > limit)
      throw std::runtime_error("Quest content collection exceeds limit");
    Number(value, 4);
  }
  void Text(std::string_view value)
  {
    Count(value.size());
    Bytes(value.data(), value.size());
  }
  void Value(const espm::QuestScriptValue& value)
  {
    if (value.type > 15 || (value.type > 5 && value.type < 11) ||
        !std::isfinite(value.real))
      throw std::runtime_error("Invalid quest content property value");
    Number(value.type, 1);
    Number(value.formId, 4);
    Number(value.alias, 2);
    Number(static_cast<uint32_t>(value.integer), 4);
    static_assert(sizeof(float) == sizeof(uint32_t) &&
                  std::numeric_limits<float>::is_iec559);
    uint32_t bits;
    std::memcpy(&bits, &value.real, sizeof(bits));
    Number(bits, 4);
    Number(value.boolean ? 1 : 0, 1);
    Text(value.text);
    Count(value.elements.size(), 1000000);
    if (value.type < 11 && !value.elements.empty())
      throw std::runtime_error("Scalar quest content value has array elements");
    for (const auto& element : value.elements) {
      if (element.type != value.type - 10)
        throw std::runtime_error("Quest content array element type mismatch");
      Value(element); // VMAD arrays are flat; the type check prevents recursion.
    }
  }
  void Scripts(const std::vector<espm::QuestScript>& scripts)
  {
    Count(scripts.size());
    for (const auto& script : scripts) {
      Text(script.name);
      Number(script.status, 1);
      Count(script.properties.size());
      for (const auto& property : script.properties) {
        Text(property.name);
        Number(property.status, 1);
        Value(property.value);
      }
    }
  }
  void Metadata(const espm::QuestScriptData& metadata)
  {
    Number(metadata.version, 2);
    Number(metadata.objectFormat, 2);
    Scripts(metadata.scripts);
    Number(metadata.hasFragments ? 1 : 0, 1);
    Number(metadata.bindVersion, 1);
    Text(metadata.fileName);
    Count(metadata.fragments.size());
    for (const auto& fragment : metadata.fragments) {
      Number(fragment.stage, 4);
      Number(fragment.logEntry, 4);
      Number(fragment.flags, 1);
      Text(fragment.script);
      Text(fragment.function);
    }
    Count(metadata.aliases.size());
    for (const auto& alias : metadata.aliases) {
      Value(alias.object);
      Number(alias.version, 2);
      Number(alias.objectFormat, 2);
      Scripts(alias.scripts);
    }
  }
  std::string Finish()
  {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256_final(&state, digest.data()) != 0)
      throw std::runtime_error("Cannot finish quest content hash");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 15]);
    }
    return result;
  }

private:
  crypto_hash_sha256_state state{};
  size_t bytes = 0;
};
}

std::string QuestJobContent::ScriptManifest(
  const std::vector<PexScript::Lazy>& scripts)
{
  Hasher hash("skymp.quest.scripts.v1");
  hash.Count(scripts.size());
  std::vector<std::pair<std::string, const PexScript::Lazy*>> ordered;
  ordered.reserve(scripts.size());
  for (const auto& script : scripts) {
    if (!script.fn)
      throw std::runtime_error("Missing quest content script loader");
    ordered.emplace_back(Name(script.source), &script);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });
  for (size_t i = 0; i < ordered.size(); ++i) {
    if (i && ordered[i - 1].first == ordered[i].first)
      throw std::runtime_error("Duplicate quest content script name");
  }
  for (const auto& [name, lazy] : ordered) {
    const auto script = lazy->fn();
    if (!script || Name(script->source) != name)
      throw std::runtime_error("Missing or mismatched quest content script");
    hash.Text(name);
    hash.Text(Digest(script->ExecutionHash()));
  }
  return hash.Finish();
}

std::string QuestJobContent::Hash(WorldState& world, uint32_t questId,
                                  const espm::QuestScriptData& metadata,
                                  const std::string& scriptManifest)
{
  Hasher hash("skymp.quest.job-content.v1");
  hash.Text(Digest(scriptManifest));
  hash.Count(world.espmFiles.size());
  for (const auto& file : world.espmFiles)
    hash.Text(file);
  const auto& masters = world.GetEspm().GetContentHashes();
  hash.Count(masters.size());
  for (const auto& master : masters) {
    hash.Text(master.fileName);
    hash.Number(master.size, 8);
    hash.Text(master.sha256);
  }
  const auto record = world.GetEspm().GetBrowser().LookupById(questId);
  if (!record.rec || record.rec->IsDeleted() || record.rec->GetType() != "QUST")
    throw std::runtime_error("Expected live quest for content hash");
  hash.Text(FormDesc::FromFormId(questId, world.espmFiles).ToString());
  hash.Number(record.rec->GetVersion(), 2);
  size_t fields = 0;
  espm::RecordHeaderAccess::IterateFields(
    record.rec,
    [&](const char* tag, uint32_t size, const char* bytes) {
      if (++fields > 1024 * 1024)
        throw std::runtime_error("Too many quest content fields");
      // A discriminant plus fixed tag and length frames every raw field. The
      // final zero terminates the sequence without buffering it to count it.
      hash.Number(1, 1);
      hash.Bytes(tag, 4);
      hash.Number(size, 4);
      hash.Bytes(bytes, size);
    },
    world.GetEspmCache());
  hash.Number(0, 1);
  // Explicit metadata may override VMAD in fixtures; pin both inputs.
  hash.Metadata(metadata);
  return hash.Finish();
}

std::string QuestJobContent::Hash(WorldState& world, uint32_t questId,
                                  const espm::QuestScriptData& metadata,
                                  const std::vector<PexScript::Lazy>& scripts)
{
  return Hash(world, questId, metadata, ScriptManifest(scripts));
}
