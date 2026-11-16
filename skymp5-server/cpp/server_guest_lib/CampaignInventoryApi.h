#pragma once
#include "papyrus-vm/VirtualMachine.h"
#include <functional>

class MpActor;
class CampaignInventoryApi
{
public:
  struct Event
  {
    uint32_t target, item, count, container;
    bool added;
  };
  using FormId = std::function<uint32_t(const VarValue&)>;
  static void Register(VirtualMachine& vm, MpActor& actor, FormId formId,
                       std::function<void(Event)> queueEvent);
};
