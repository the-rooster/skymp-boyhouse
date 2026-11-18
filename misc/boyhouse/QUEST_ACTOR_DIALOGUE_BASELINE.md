# Actor dialogue in the Bannered Mare

## Timed private dialogue

Two headless clients received the six authored topics of `DialogueWhiterunBanneredMareScene1` in order, on their projected Hulda and Mikael copies. Delivery started about 0.03 seconds into the test and the final audio slot ended at 35.45 seconds. With 32 concurrent connections, both party members received all six cues. The player in a separate instance of the inn and the 29 players in shared space received zero. A pending visitor and a kicked visitor also received zero cues in their respective checks.

The startup test used a temporary route to the original quest and filled all four roles from its authored aliases in either event order. The network test resumed a running quest with seeded role bindings and invoked the new trusted server scene driver. The native startup probe rolls back at the playback boundary. Ordinary play still encounters the original Story Manager selection blocker. No rendered client was used.

| Measured coverage | Count |
| --- | ---: |
| Winning scenes parsed across five masters | 2,130 |
| Actor-dialogue quests with an auto-start scene | 793 |
| Auto-start scenes with only dialogue actions | 787 |
| Scenes meeting the first planner's authored structure and role requirements | 132 |
| Structurally eligible scenes with English timings for every topic | 122 |
| Original scenes exercised through timed multiplayer packet delivery | 1 |

Across the five masters the offline generator discovered 8,784 distinct scene-action topics and measured 6,731 from the installed English archive. The 132-scene structural result assumes valid saved speaker bindings and live campaign copies. It excludes 346 scenes at the phase layout, 230 at scene fields or flags, 74 at action controls, seven at conditions or scripts, three at conditional phases and one at actor flags. Each Bannered Mare topic has two possible INFO records. The server synchronizes the topic and its time slot. Matching audible responses across game clients remains unverified.

See the [setup, trusted API and 32-client test](QUESTS.md#party-dialogue-playback).

## Ordered Story Manager selection

The headless server tested Hulda speaking to Mikael in the Bannered Mare against all 900 actor-dialogue quest records. The original story tree did not start a conversation for this encounter. Of the 900, **891** can now be ruled out for this pair. Six need a condition the server cannot yet evaluate. Three remain possible at the first unresolved Story Manager decision.

| First observation | Previous run | Ordered selection and alias checks |
| --- | ---: | ---: |
| Story conditions excluded the encounter | 772 | 871 |
| A required quest alias excluded the participants | 0 | 20 |
| Condition or run-on context still unsupported | 94 | 6 |
| Story Manager selection still pending | 34 | 3 |
| Test errors | 0 | 0 |

Of the 94 previously blocked on conditions, 82 now fail a story condition, five fail a required alias filter, one reaches selection and six still need condition support. Those six involve `GetStage` (three), `GetInCurrentLoc` with an unsupported run-on context (two) and `GetIsVoiceType` (one). Of the 34 previously waiting on selection, 17 now have every candidate route ruled out, 15 fail a required alias filter and two still reach selection.

Starting from the NPC pair alone, in either speaking order, the original tree reaches `GenericScenesSpecial` (Skyrim 0x016FA1). Its quest entry for `DialogueGenericSceneSpecial01` (Skyrim 0x016FA6) has flags whose selection behavior is unresolved. This same decision gates three candidates when Hulda speaks first, namely `DialogueGenericSceneSpecial01`, `DialogueWhiterunBanneredMareScene1` and `DialogueGenericScene04`.

**An original quest reached its first scene in an isolated route test.** A temporary plugin pointed the event directly at `DialogueWhiterunBanneredMareScene1` (Skyrim 0x017F32). Skyrim.esm supplied the original quest and scene. In both speaking orders the quest filled Actor1 and Actor2 from the event, reused those campaign identities for its named Hulda and Mikael aliases, then requested scene 0x017F33. The timed playback test above exercises that scene separately. The route override is required to reach this result today.

Across the 900 dialogue quests, 793 have an automatically starting scene. Of those 793 scenes, 787 contain only dialogue actions. The five masters specify 3,856 dialogue actions across these auto-start scenes. This count identifies how many original records share the playback requirement. Each scene still has its own conditions, topics, participants and callbacks to verify.

Separate temporary two-quest fixtures verified sibling priority, fallback when a required actor filter rejects, and rejection when a quest forbids reuse of the same actor. The probes verify that journals and private NPC copies survive the attempts intact. Reversing the catalog input reproduced all 2,274 per-quest observations. All 1,374 forced-start controls matched. Across the full forced-start catalog 27 records completed startup and three executed an authored stage. Zero quests have a verified complete multiplayer playthrough.

## Previous editor-location and faction run

The headless server attempted all 2,274 winning quest records from Skyrim, Update, Dawnguard, HearthFires and Dragonborn. Exactly 900 requested the Story Manager's actor-dialogue event (`ADIA`). The scenario used Hulda as actor 1 and Mikael as actor 2. Each attempt used a fresh personal campaign and private copies of their original placed references, at their authored positions. No dialogue quest started in this encounter.

| First observation for the 900 dialogue quests | Before editor-location and faction support | After |
| --- | ---: | ---: |
| An authored condition evaluated false for this encounter | 187 | 772 |
| The server could not evaluate a required condition | 681 | 94 |
| The Story Manager still needed to choose a route | 32 | 34 |
| Probe error | 0 | 0 |

Both columns used the same NPC originals, order, location and owned game-file fingerprints. Of the 587 quests whose first unsupported condition was resolved, 585 rejected this encounter. Two reached the Story Manager selection step, `DialogueGenericSceneSpecial01` and `DialogueGenericScene04`. Inappropriate participants or locations are expected to rule out authored conversations.

Of the 772 exclusions, 535 first failed an editor-location condition, 184 a current-location condition, 50 a faction condition and three an NPC identity condition. Hulda and Mikael both have Whiterun as their persistence location. Their unique-actor records and original interior cell identify the Bannered Mare as their editor location.

Of the 94 unimplemented condition results, 48 first require `GetInCell`, 16 require `GetIsCurrentPackage`, 15 require `GetGlobalValue`, and 11 require `GetStage`. Four need other functions or run-on contexts. The 34 selection results include 17 quests with multiple candidate entries, 15 requiring sibling selection, and two with an unresolved previous-sibling chain.

The 1,374 other quest records repeated the forced-start scenario. All 1,374 controls matched. Across the full catalog, 27 completed forced startup and three of those executed an authored stage. Zero complete multiplayer playthroughs have been verified.

## Reproducing the result

See the [bulk startup commands](QUESTS.md#actor-dialogue-across-the-five-masters). `--scenario actor-dialogue` selects the 900 records using their winning event fields. `--contrast` compares them to the forced-start run and checks the other records as controls. `--compare` compares successive runs of the same fixture.

The per-quest JSON records the original actor references, location, candidate count, ordered route nodes for unique candidates, stages reached, and the first observed reason execution stopped. Automatic conversation triggers, sibling and random selection, event aliases, scene playback and party playthroughs still require implementation or verification.
