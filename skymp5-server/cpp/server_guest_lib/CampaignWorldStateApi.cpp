#include "CampaignWorldStateApi.h"
#include "CampaignReferenceBinding.h"
#include "MpActor.h"
#include "QuestJournal.h"
#include "WorldState.h"

void CampaignWorldStateApi::Register(VirtualMachine& vm, MpActor& actor,
                                     FormId formId)
{
  vm.RegisterFunction(
    "ObjectReference", "IsDisabled", FunctionType::Method,
    [&, formId](VarValue self, std::vector<VarValue> args) {
      if (!args.empty())
        throw std::invalid_argument("IsDisabled takes no arguments");
      auto& reference = CampaignReferenceBinding::Resolve(actor, formId(self));
      return VarValue(
        QuestJournal::ReadReference(actor, reference).isDisabled);
    });
  for (const auto& [name, disabled] :
       { std::pair{ "Enable", false }, std::pair{ "Disable", true },
         std::pair{ "EnableNoWait", false },
         std::pair{ "DisableNoWait", true } }) {
    vm.RegisterFunction(
      "ObjectReference", name, FunctionType::Method,
      [&, formId, disabled](VarValue self, std::vector<VarValue> args) {
        if (args.size() != 1 || args[0].GetType() != VarValue::kType_Bool)
          throw std::invalid_argument(
            "Visibility change requires one fade boolean");
        if (static_cast<bool>(args[0]))
          throw std::runtime_error(
            "Campaign visibility fade requires durable engine effects");
        const auto original = formId(self);
        auto& reference = CampaignReferenceBinding::Resolve(actor, original);
        QuestJournal::SetReferenceDisabled(actor, reference, disabled);
        return VarValue::None();
      });
  }
}
