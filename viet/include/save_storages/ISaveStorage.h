#pragma once
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Viet {

template <typename T, typename FormDescType, typename FilterType>
class ISaveStorage
{
public:
  using IterateSyncCallback = std::function<void(const T&)>;
  using UpsertCallback = std::function<void()>;
  // nullptr means the submitted batch succeeded. A failure retains its
  // original exception, including exceptions not derived from std::exception.
  using UpsertResultCallback = std::function<void(std::exception_ptr)>;
  using IterateCallback = std::function<void(const std::vector<T>&)>;

  virtual ~ISaveStorage() = default;

  virtual bool SupportsAtomicBatches() const noexcept { return false; }
  virtual bool SupportsUpsertResults() const noexcept { return false; }

  virtual void IterateSync(const IterateSyncCallback& cb) = 0;
  virtual void Upsert(std::vector<std::optional<T>>&& changeForms,
                      const UpsertCallback& cb) = 0;
  // Opt-in task-specific results, delivered by Tick. Implementations without
  // support must not submit work or silently fall back to untracked errors.
  // A synchronous throw means no write remains queued, unless its terminal
  // result was already delivered inline. Delivery must use the Tick/caller
  // thread, never a worker; after a result, that attempt does no further
  // writes.
  virtual void UpsertWithResult(std::vector<std::optional<T>>&&,
                                const UpsertResultCallback&)
  {
    throw std::runtime_error("Save storage does not support upsert results");
  }
  virtual void Iterate(const IterateCallback& cb,
                       const std::optional<FilterType>& filter) = 0;
  virtual uint32_t GetNumFinishedUpserts() const = 0;
  virtual uint32_t GetNumFinishedIterates() const = 0;

  // Calls all callbacks that are ready to be called.
  // Basic exception guarantee: if a callback throws, it'll be deleted.
  // Other callbacks will still be called next time Tick is called.
  virtual void Tick() = 0;

  virtual bool GetRecycledChangeFormsBuffer(
    std::vector<std::optional<T>>& changeForms) = 0;

  virtual const std::string& GetName() const = 0;
};

namespace ISaveStorageUtils {
template <typename T, typename FormDescType, typename FilterType>
inline uint32_t CountSync(ISaveStorage<T, FormDescType, FilterType>& storage)
{
  uint32_t n = 0;
  storage.IterateSync([&](auto) { ++n; });
  return n;
}

template <typename T, typename FormDescType, typename FilterType>
inline std::optional<T> FindSync(
  ISaveStorage<T, FormDescType, FilterType>& storage,
  const FormDescType& formDesc)
{
  std::optional<T> res;
  storage.IterateSync([&](const T& changeForm) {
    if (changeForm.formDesc == formDesc)
      res = changeForm;
  });
  return res;
}

template <typename T, typename FormDescType, typename FilterType>
inline std::map<FormDescType, T> FindAllSync(
  ISaveStorage<T, FormDescType, FilterType>& storage)
{
  std::map<FormDescType, T> res;
  storage.IterateSync(
    [&](const T& changeForm) { res[changeForm.formDesc] = changeForm; });
  return res;
}
}

}
