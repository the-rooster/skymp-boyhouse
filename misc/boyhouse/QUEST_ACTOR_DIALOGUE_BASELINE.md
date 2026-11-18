# Actor dialogue in the Bannered Mare

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
