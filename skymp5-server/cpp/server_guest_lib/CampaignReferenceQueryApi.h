#pragma once
#include "papyrus-vm/VirtualMachine.h"
#include <functional>

class MpActor;

// Read-only physical queries for the isolated campaign VM. Logical identities
// resolve only to the owner or an explicitly bound private reference. Reads
// participate in QuestJournal's reference transaction and its conflict checks.
class CampaignReferenceQueryApi
{
public:
  using FormId = std::function<uint32_t(const VarValue&)>;
  using Resolve = std::function<std::shared_ptr<IGameObject>(uint32_t)>;

  // GetDistance requires one shared coordinate frame. GetParentCell requires
  // an authoritative CELL descriptor; a WRLD descriptor alone cannot identify
  // an exterior CELL. Both unsupported cases throw instead of inventing data.
  static void Register(VirtualMachine& vm, MpActor& actor, FormId formId,
                       Resolve resolve);
};
