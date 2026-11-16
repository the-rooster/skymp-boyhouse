#pragma once
#include "papyrus-vm/VirtualMachine.h"
#include <functional>

class MpActor;

// Partial, synchronous authoritative visibility semantics. Fade, enable-parent
// propagation, occupied references and outstanding actor effects are refused.
// Actual publication requires the world's configured projection barrier.
class CampaignWorldStateApi
{
public:
  using FormId = std::function<uint32_t(const VarValue&)>;
  static void Register(VirtualMachine& vm, MpActor& actor, FormId formId);
};
