# Quest dependency backlog

Generated static analysis. Counts include internal/helper quests and potential dependencies in all attached functions/states, including unreachable code. Counts overlap and do not predict how many quests a feature makes playable.

- Quest records: **2274**.
- Parsed PEX objects: **14026**.
- Quests with explicit unresolved dependencies: **0**.
- Quests with runtime-dependent dispatch sites: **1146**.
- Record IDs not indexed: **1** (listed in JSON).
- Certified playable quests: **0**. Every active catalog entry remains untested.

## Missing shared systems

| Capability | Quest records affected |
| --- | ---: |
| condition.non-stage-evaluation | 2118 |
| alias.reference | 1978 |
| engine.dialogue | 1865 |
| world.campaign-references | 1810 |
| engine.story-manager | 1424 |
| alias.fill.unique-actor | 1378 |
| engine.scenes | 1309 |
| engine.events | 1296 |
| alias.fill.event | 1180 |
| forms.asset-bindings | 1108 |
| condition.run-on:7 | 1062 |
| alias.properties | 1058 |
| quest.startup-stages | 726 |
| alias.scripts | 699 |
| engine.packages | 592 |
| alias.fill.forced-reference | 547 |
| alias.fill.matching-or-script | 520 |
| world.campaign-player | 493 |
| alias.location | 486 |
| quest.start-game-enabled | 423 |
| condition.run-on:0:physical | 403 |
| quest.run-once | 402 |
| alias.fill.created-reference | 384 |
| quest.shutdown-stages | 378 |
| condition.run-on:2 | 301 |
| condition.run-on:5 | 277 |
| alias.fill.location-or-alias | 265 |
| condition.flags | 255 |
| condition.run-on:1 | 164 |
| alias.fill.specific-location | 142 |
| alias.fill.matching-reference | 138 |
| quest.log-flags | 72 |
| alias.fill.external-alias | 63 |
| condition.run-on:3 | 31 |
| quest.keep-instance-data | 22 |
| condition.run-on:6 | 7 |
| condition.run-on:4 | 3 |

## Most widely referenced missing native APIs

| API | Quest records affected |
| --- | ---: |
| quest.stop | 877 |
| utility.wait | 444 |
| actor.setactorvalue | 283 |
| actor.getactorvalue | 270 |
| scene.start | 264 |
| actor.evaluatepackage | 256 |
| objectreference.moveto | 250 |
| actor.startcombat | 247 |
| debug.trace | 241 |
| actor.kill | 210 |
| referencealias.forcerefto | 210 |
| actor.setrelationshiprank | 205 |
| objectreference.delete | 199 |
| actor.stopcombat | 198 |
| game.addachievement | 192 |
| objectreference.addtomap | 191 |
| form.registerforupdategametime | 186 |
| actor.setalpha | 164 |
| cell.isattached | 163 |
| alias.registerforsingleupdate | 160 |
| actor.attachashpile | 146 |
| actor.setcriticalstage | 146 |
| game.incrementstat | 146 |
| alias.registerforsinglelosgain | 145 |
| keyword.sendstoryevent | 133 |
| form.unregisterforupdate | 124 |
| scene.stop | 119 |
| objectreference.getcurrentlocation | 118 |
| quest.start | 114 |
| location.ischild | 98 |

## Most widely referenced missing condition functions

| Condition | Quest records affected |
| --- | ---: |
| GetIsID (72) | 1467 |
| GetInCurrentLoc (359) | 1058 |
| GetIsEditorLocation (565) | 634 |
| GetIsAliasRef (566) | 416 |
| GetEventData (576) | 393 |
| GetInCell (67) | 351 |
| GetDistance (1) | 244 |
| GetIsVoiceType (426) | 242 |
| LocationHasKeyword (562) | 219 |
| IsSceneActionComplete (550) | 199 |
| HasRefType (561) | 169 |
| IsInDialogueWithPlayer (249) | 144 |
| GetActorValue (14) | 126 |
| GetInCurrentLocAlias (360) | 118 |
| IsLinkedTo (650) | 117 |
| GetPCIsSex (131) | 105 |
| GetInWorldspace (310) | 100 |
| IsInInterior (300) | 93 |
| GetRandomPercent (77) | 92 |
| HasKeyword (560) | 91 |

## Limits and next steps

- Static discovery cannot prove branch reachability, correct behavior or playability.
- Declared receiver types do not enumerate all virtual overrides or runtime object types.
- Runtime-created forms, computed IDs, native-engine side effects and dynamic events need traces.
- World-reference/asset record scripts and non-script relationships are not traversed yet.
- Cross-quest record links are reported separately, not expanded into every linked quest.
- Client projection, phase transitions, lifecycle, persistence and real-client validation remain gates.

Implement shared capabilities, then rerun this report and the regression suites. Use runtime traces and real playthroughs to resolve unknowns; a dependency report must never automatically enable a quest adapter.

## Reproducibility

Capability registry SHA-256: ed69ae468b7a3fd6825698d3a9f9dc84ebc16d70523f818813f5c1896d228deb

- Skyrim.esm: e198c3b85e5e48e0c92a6580d8f66e644256b68d812ee32b61735cf9b753df73
- Update.esm: b298b0f65fa0127fd13c1f5ef6bb30eaf9d21fd19f5e6f8f91f829c9007d3280
- Dawnguard.esm: fc8f92cb7ed55217046ee96b84caa54c058c12f7c408ac2a71da880a7bd3eab7
- HearthFires.esm: 0f681496771e6d1983c6c1c9dd6602964068837f015bfa21e69a149c3b050047
- Dragonborn.esm: 635f21a938a17b8677f524cf41210933afc7f23820f2e210ab3bb6d3cd837046
- Skyrim - Misc.bsa: 30957adeb1a109c8b8079a5791c2a24c1fdd1d01957e9338480f4d8e03d9d23f
