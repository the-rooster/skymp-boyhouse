#include "libespm/Loader.h"

#include "AllocatedBuffer.h"
#include "MappedBuffer.h"
#include "libespm/Utils.h"
#include <algorithm>
#include <array>
#include <sodium.h>

namespace espm {

Loader::Loader(const fs::path& dataDir, const std::vector<fs::path>& fileNames,
               OnProgress onProgress, BufferType bufferType_)
  : Loader(MakeFilePaths(dataDir, fileNames), onProgress, bufferType_)
{
}

Loader::Loader(const std::vector<fs::path>& filePaths_, OnProgress onProgress,
               BufferType bufferType_)
  : filePaths(filePaths_)
  , bufferType(bufferType_)
{
  for (const auto& p : filePaths) {
    entries.emplace_back();
    auto& entry = entries.back();

    const auto was = std::chrono::steady_clock::now();
    entry.buffer = MakeBuffer(p);
    entry.fileName = p.filename().string();
    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<float> elapsedTime = end - was;
    entry.readDuration = elapsedTime.count();
    entry.size = entry.buffer->GetLength();

    const auto was1 = std::chrono::steady_clock::now();
    entry.browser.reset(
      new Browser(entry.buffer->GetData(), entry.buffer->GetLength()));
    const auto end1 = std::chrono::steady_clock::now();
    const std::chrono::duration<float> elapsedTime1 = end1 - was1;
    entry.parseDuration = elapsedTime1.count();
    if (onProgress) {
      onProgress(entry.fileName.string(), entry.readDuration,
                 entry.parseDuration, entry.size);
    }
  }
  combiner = std::make_unique<Combiner>();
  for (auto& entry : entries) {
    const auto fileName = entry.fileName.string();
    combiner->AddSource(entry.browser.get(), fileName.c_str());
  }
  combineBrowser = combiner->Combine();
}

const CombineBrowser& Loader::GetBrowser() const noexcept
{
  return *combineBrowser;
}

std::vector<std::string> Loader::GetFileNames() const noexcept
{
  std::vector<std::string> res;
  res.reserve(filePaths.size());
  for (auto& p : filePaths) {
    res.push_back(p.filename().string());
  }
  return res;
}

std::map<std::string, Loader::FileInfo> Loader::GetFilesInfo() const
{
  std::map<std::string, FileInfo> res;

  for (const auto& entry : entries) {
    auto hash = utils::CalculateHashcode(entry.buffer->GetData(),
                                         entry.buffer->GetLength());
    res.emplace(entry.fileName.string(),
                FileInfo{ hash, entry.buffer->GetLength() });
  }

  return res;
}

const std::vector<Loader::FileContentHash>& Loader::GetContentHashes() const
{
  std::call_once(contentHashesOnce, [&] {
    std::vector<FileContentHash> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
      crypto_hash_sha256_state hash{};
      if (crypto_hash_sha256_init(&hash) != 0)
        throw std::runtime_error("Cannot initialize master content hash");
      const auto size = entry.buffer->GetLength();
      const auto* data = reinterpret_cast<const unsigned char*>(
        entry.buffer->GetData());
      for (size_t offset = 0; offset < size;) {
        const auto count = std::min<size_t>(size - offset, 1024 * 1024);
        if (crypto_hash_sha256_update(&hash, data + offset, count) != 0)
          throw std::runtime_error("Cannot update master content hash");
        offset += count;
      }
      std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
      if (crypto_hash_sha256_final(&hash, digest.data()) != 0)
        throw std::runtime_error("Cannot finish master content hash");
      constexpr char hex[] = "0123456789abcdef";
      std::string encoded;
      encoded.reserve(digest.size() * 2);
      for (const auto byte : digest) {
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
      }
      result.push_back({ entry.fileName.string(), uint64_t(size),
                         std::move(encoded) });
    }
    contentHashes.swap(result);
  });
  return contentHashes;
}

std::vector<fs::path> Loader::MakeFilePaths(
  const fs::path& dataDir, const std::vector<fs::path>& fileNames)
{
  std::vector<fs::path> res;
  for (auto& fileName : fileNames) {
    res.push_back(dataDir / fileName);
  }
  return res;
}

std::unique_ptr<Viet::IBuffer> Loader::MakeBuffer(
  const fs::path& filePath) const
{
  switch (bufferType) {
    case BufferType::AllocatedBuffer:
      return std::make_unique<Viet::AllocatedBuffer>(filePath);
    case BufferType::MappedBuffer:
      return std::make_unique<Viet::MappedBuffer>(filePath);
    default:
      throw std::runtime_error("[espm] unhandled buffer type");
  }
}

} // namespace espm
