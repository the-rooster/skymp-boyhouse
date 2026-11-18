#pragma once
#include <cstdint>

class MpActor;

// Returns a global LCTN ID for a validated personal copy of an interior
// placed actor. Throws when the original and its unique-NPC data cannot
// establish the editor location.
uint32_t ActorDialogueEditorLocation(MpActor& actor);
