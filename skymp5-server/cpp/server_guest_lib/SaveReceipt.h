#pragma once
#include "FormDesc.h"
#include <cstdint>
#include <exception>

class WorldState;

// Server-thread observation of one immutable save batch. This is not a
// persistent job ID: recovery still reads the job marker saved with its
// effects.
class SaveReceipt
{
public:
  enum class State
  {
    Pending,
    Durable,
    Abandoned
  };
  State GetState() const noexcept { return state; }
  uint64_t GetSequence() const noexcept { return sequence; }
  uint64_t GetAttempts() const noexcept { return attempts; }
  std::exception_ptr GetLastError() const noexcept { return lastError; }
  const FormDesc& GetCampaign() const noexcept { return campaign; }
  uint64_t GetJournalRevision() const noexcept { return journalRevision; }

private:
  friend class WorldState;
  State state = State::Pending;
  uint64_t sequence = 0, attempts = 0, journalRevision = 0;
  FormDesc campaign;
  std::exception_ptr lastError;
};
