#include "ActorDialogueEditorLocation.h"
#include "CampaignReferenceBinding.h"
#include "TestUtils.hpp"

extern espm::Loader& GetEspmLoader();

TEST_CASE(
  "Dialogue resolves placed actors' editor locations across campaign copies",
  "[ActorDialogueEditorLocation][espm]")
{
  PartOne server;
  server.AttachEspm(&GetEspmLoader());
  auto& world = server.worldState;
  constexpr uint32_t ownerId = 0xff008010;
  server.CreateActor(ownerId, {}, 0, 0x3c);
  auto& owner = world.GetFormAt<MpActor>(ownerId);
  const auto phase =
    "campaign:" + std::to_string(ownerId) + ":editor-location";
  owner.SetWorldInstance(phase);
  // Copies currently stand in Tamriel. Their original ACHR persistence is
  // Whiterun (0x18a56), and their editor location is the Bannered Mare.
  auto add = [&](uint32_t id, uint32_t base) -> MpActor& {
    world.AddForm(
      std::make_unique<MpActor>(
        LocationalData{ {}, {}, FormDesc::FromFormId(0x3c, world.espmFiles) },
        FormCallbacks::DoNothing(), base),
      id);
    auto& copy = world.GetFormAt<MpActor>(id);
    copy.SetWorldInstance(phase);
    return copy;
  };
  auto& hulda = add(ownerId + 1, 0x13ba3);
  auto& mikael = add(ownerId + 2, 0x1a670);
  REQUIRE_THROWS_AS(ActorDialogueEditorLocation(hulda), std::runtime_error);
  CampaignReferenceBinding::Bind(owner, 0x1a66e, hulda);
  CampaignReferenceBinding::Bind(owner, 0x1a671, mikael);

  const auto saved = MpChangeForm::ToJson(hulda.GetChangeForm());
  REQUIRE(ActorDialogueEditorLocation(hulda) == 0x1f86d);
  REQUIRE(ActorDialogueEditorLocation(mikael) == 0x1f86d);
  REQUIRE(MpChangeForm::ToJson(hulda.GetChangeForm()) == saved);
  REQUIRE_FALSE(world.LookupFormByIdNoLoad(0x1a66e));
  REQUIRE_FALSE(world.LookupFormByIdNoLoad(0x1a671));
  auto& guard = add(ownerId + 3, 0x5eab5);
  CampaignReferenceBinding::Bind(owner, 0x5ea70, guard);
  REQUIRE_THROWS_WITH(ActorDialogueEditorLocation(guard),
                      "Original ACHR has no persistence location");
  hulda.SetWorldInstance("");
  REQUIRE_THROWS_AS(ActorDialogueEditorLocation(hulda), std::runtime_error);
}
