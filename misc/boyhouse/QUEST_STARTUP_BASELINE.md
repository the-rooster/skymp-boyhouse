# Quest startup results

Attempted **2274** quest records. **27** completed the explicit startup probe, **2247** reached a reported blocker, and **0** returned a test error.

Each attempt requests a first start in a fresh campaign and rolls back its changes. Natural triggers, later quest branches and multiplayer playthroughs require separate tests.

Of the completed probes, **3** executed at least one authored startup stage. **24** completed without executing a stage.

## First startup blockers

| Cause | Quests | Steps | Example quests |
| --- | ---: | --- | --- |
| story.event | 1404 | admission | DLC1_WESC03, DLC1_WESC04, DLC1_WESC05 |
| quest.start-game-enabled | 418 | admission | DLC1NPCMentalModel, DLC1Init, DLC1Radiant |
| alias.fill.unique-actor | 176 | aliases | DLC1VQ07, DLC1VQ02, DLC1VQ01 |
| alias.fill.forced-reference | 121 | aliases | DLC1_WESC06_CacheTracking, DLC1_WESC07_CacheTracking, DLC1_WESC08_CacheTracking |
| alias.fill.world-query | 32 | aliases | DLC2MH01, DLC2dunNchardakReset, DLC2MQ05SkaalSceneFiller |
| alias.fill.external-alias | 30 | aliases | DLC1VQ04, DLC1VQ05, DLC1VQ06 |
| alias.flags | 14 | aliases | DLC1PlayerVampireQuest, DLC1VQDragonMisc, DLC2SV01 |
| quest.start-flags | 11 | admission | DLC2MQ01, DLC2MHRieklingHandler, TG00SP |
| alias.fill.matching-or-script | 10 | aliases | DLC2BookDungeonController, DLC2dunNchardakTracking, DLC2Pillar |
| quest.implicit-startup-stage | 10 | startup-stages | DA01FIN, MQ302Test, TGDialogueKarliah |
| story.conditions | 8 | admission | DLC01SoulCairnHorseQuest, DLC1VQ05DragonFight, DLC1SoulCairnHuskMerchant |
| alias.reserve | 4 | aliases | DLC1VQ03Hunter, DLC2MQ04, DLC2MQ06 |
| alias.effect.VTCK | 2 | aliases | PlayerWerewolfQuest, MS12PostQuest |
| alias.effect.ALFC | 1 | aliases | BYOHRelationshipAdoption |
| native.cell.reset | 1 | startup-stages | DLC2RR03Mine |
| native.form.registerforupdate | 1 | startup-stages | DBRecurringControlQuest |
| native.objectreference.placeatme | 1 | startup-stages | DLC1HunterBaseIntroAmbush |
| property.read.objectreference.x | 1 | startup-stages | DLC1NPCMonitoringPlayer |
| property.write.globalvariable.value | 1 | startup-stages | TGCrownPerk |
| scene.begin-on-quest-start | 1 | scenes | DA05FuneralSpectatorsQuest |

## Test errors

None observed.

## Story event requests

| Diagnostic | Quests |
| --- | ---: |
| Quest requires story event ADIA | 900 |
| Quest requires story event SCPT | 262 |
| Quest requires story event CLOC | 172 |
| Quest requires story event KILL | 15 |
| Quest requires story event CRFT | 11 |
| Quest requires story event LEVL | 8 |
| Quest requires story event AIPL | 5 |
| Quest requires story event ASSU | 5 |
| Quest requires story event CAST | 5 |
| Quest requires story event REMP | 5 |
| Quest requires story event JAIL | 3 |
| Quest requires story event AHEL | 2 |
| Quest requires story event ARRT | 2 |
| Quest requires story event CHRR | 2 |
| Quest requires story event DEAD | 2 |
| Quest requires story event ESJA | 2 |
| Quest requires story event SKIL | 2 |
| Quest requires story event NVPE | 1 |
