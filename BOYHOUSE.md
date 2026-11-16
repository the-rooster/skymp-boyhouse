# Boyhouse SkyMP fork

Experimental work on personal campaigns in a shared Skyrim world. Upstream baseline: `2849e676fe008925384b26f34476e382a30bd877`. Target: 16-32 players; the laboratory uses four slots and has not been load-tested.

## Implemented boundary prototype

- Persistent `worldInstance` on server references, with missing fields in old saves defaulting to shared space.
- Native subscription filtering sends ordinary destroy/create messages when a character changes instance. Movement, hosting, hits, spell targets, activation, crafting, and inventory transfers check the boundary. Spawned references inherit their source instance.
- Server-owned parties: explicit invitations, acceptance, private entry, kicks, disbanding, and safe guest recovery after restart. Identical histories never grant strangers admission.
- Opt-in `boyhouseInstances.experimental` configuration, registered areas, and authenticated connection-to-character resolution for requests.
- Development console commands in the forked client; no polished party UI yet.
- Owned-master quest inventory scanner with overrides, compressed records, source hashes, and an explicit untested status.

## Automated quest dependency analysis

The [catalog analyzer](misc/boyhouse/DEPENDENCIES.md) scans all five owned masters and the original PEX archive, follows typed script calls, and ranks missing shared capabilities. See the [generated backlog](misc/boyhouse/QUEST_BACKLOG.md). Reports preserve unknowns and runtime-dispatch limits; they never enable adapters or certify quests.

## Quest journal and fragment runtime

Persistent personal/host journals, 23 Papyrus Quest state methods, four quest condition predicates, expired-context guards, and a read-only client journal feed are implemented. The new `questExecuteStage` trusted server API loads original PEX fragments into a campaign-scoped VM and commits journal changes and script variables together. It remains gated by experimental per-quest adapters; no complete vanilla quest is enabled. `mp "bh journal"` displays the diagnostic projection. See [the implementation contract and remaining gates](misc/boyhouse/QUESTS.md).

## Dialogue/scene owner lookup verification on 2026-11-16 UTC

- Scene and TopicInfo `GetOwningQuest` resolve to the active campaign, including guest-triggered objective changes. Package-action fields and parent-topic ownership are handled separately. Dialogue/scene execution remains unfinished.
- Release build, **78 instance/quest cases with 89,450 assertions**, 25 JavaScript/TypeScript tests, 31 Python tests and both server smoke checks passed. The catalog report was regenerated; no quest status changed.

## Current alias/form verification on 2026-11-16 UTC

- Linux Release server/addon build and full native suite passed: **331 cases, 565,315 assertions**. Combined instance/quest suite: **77 cases, 89,430 assertions**.
- Campaign form/player identities and alias VM objects now resolve inside the isolated runner. Deterministic forced reference/location, unique actor and external-alias targets are a partial implementation; no live NPC/reference clones are created.
- Alias bindings and attached alias-script variables persist in VM schema 4, migrate older saves, stay with the host during guest play, and roll back with the journal on failure. Quest reset removes its own bindings and variables.
- The bounded parser reads all owned alias definitions: **14,514 reference aliases and 1,237 location aliases**. Parsing includes conditions and effects that the runtime does not implement yet.
- **25 JavaScript/TypeScript tests and 31 Python tests passed**. Native addon and full-server protocol smoke checks passed. The full catalog report was regenerated against the audited runtime sources; all 2,274 quest records remain untested.
- **Zero quests have certified multiplayer playthroughs.** World overlays/effects, complete alias filling and lifecycle, dialogue/scenes, latent persistence and a compatible tested native client remain required.

## Earlier globals verification on 2026-11-16 UTC

- Linux Release server/addon build and full native suite passed: **311 cases, 519,173 assertions**. Focused quest suite: **48 cases, 41,984 assertions**.
- Nested stage calls across quests now execute in the same campaign transaction and share the instruction budget. Failures roll back all affected stages and variables.
- Shared Papyrus arrays retain allocation identity across scripts, character reloads and quest resets; null/equal-valued allocations stay distinct. Save migration and collection are tested.
- Campaign globals persist independently, use the host's values during guest play, and participate in rollback. Original DA08 stage-60 conditions are fixtures for observing earlier writes in a nested stage; the quest itself is not playable.
- The 25 JavaScript/TypeScript tests and four catalog tests passed during this change set. Native addon and full server protocol smoke checks passed against the final binary.
- **Zero quests have certified multiplayer playthroughs.** Alias filling, lifecycle/events, NPC/world effects, dialogue/scenes, latent VM persistence and native client projection remain required for the full five-master campaign.

## Earlier fragment milestone verification on 2026-11-16 UTC

- Linux Release server/addon build passed. Full native suite: **290 cases, 518,484 assertions passed**.
- Tests parse every QUST VMAD in the five owned masters and every PEX in the owned vanilla script archive.
- Original MQ102 stage 100 and DA04 stage 5 execute. DA04's original fragment displays objective 5 in the host's journal when invoked by a guest; leaving restores the guest's personal history. Unsupported map-marker/alias/lifecycle operations roll back.
- Persistent variables, arrays, quest reset, conditional log entries, stale identities, property targets, instruction quotas and transaction rollback have regression coverage. Shared array identities and latent continuations remain unsupported and fail explicitly.
- 25 JavaScript/TypeScript tests and 4 Python catalog tests passed. No client code changed in this milestone.
- The focused instance/quest suite passed 36 cases and 41,857 assertions. Native addon/party/original-fragment integration and full-server protocol smoke passed, including rejected client writes, host-only effects, guest removal and reconnect.
- These are server execution fixtures, not completed quests. **Zero quests have certified multiplayer playthroughs.** See the [native client runtime audit](misc/boyhouse/CLIENT_RUNTIME.md) for concrete 1.7.104 port requirements.

## Earlier journal milestone verification on 2026-11-16 UTC

- Linux Release server/addon and server TypeScript build passed; the client webpack/typecheck also passed.
- Full native suite: 271 cases and 476,285 assertions passed. The focused instance/quest suite passed 17 cases and 115 assertions, including 8 new quest cases.
- 25 JavaScript/TypeScript tests and 4 Python catalog tests passed.
- Native addon/party/journal-feed integration and full-server protocol smoke passed, including rejected forged quest writes, host-only progress, guest removal, and journal reconnect.
- Quest coverage remains zero certified playthroughs. MQ102 appears only as a record-index fixture; its vanilla scripts, dialogue, and rewards were not executed. The ESM hash and distribution-content tests self-skip outside CI.

## Earlier boundary milestone verification

- Linux Release server and addon build completed on boyhouse.
- Full native suite: 263 cases, 476,977 assertions passed; includes 9 new instance cases with 40 assertions.
- 14 party-rule tests, 2 client command tests, and 4 catalog parser tests passed.
- Addon smoke test with all five owned masters passed. The full-server bot protocol test also passed login, spoofed-sender rejection, invite/accept/entry, kick, disconnect, and reconnect. It exposed and now covers an upstream bot lifetime fix. These are simulated connections, not Skyrim clients.
- Server TypeScript, client webpack/typecheck, and Windows Release Skyrim Platform DLL/CEF builds passed. Use the existing Yarn lockfile (`yarn install --frozen-lockfile`); an unlocked npm resolution pulls incompatible Node typings for the older TypeScript compiler.
- Compose initialization and verification passed; the loopback staging service returned HTTP 200 for its manifest and Docker reported healthy.
- The upstream distribution-content test self-skips outside CI; packaging is not certified. No in-game clients or quests have been verified. Runtime compatibility remains an explicit open gate.

## What this does not implement yet

This is not full campaign multiplayer or a finished instancing engine. Vanilla static references are not cloned per campaign. General quest execution, complete alias filling/effects, dialogue scenes, story events, native client journals, branch ownership, world overlays, transactions spanning inventory/world effects, transition acknowledgements, automatic boundary crossing, guest progression rules, and client reset/rebinding remain to be built and tested. Entering an experimental instance can hide shared NPCs and interactive references until overlays exist. Use the empty boundary laboratory only.

The full-release target is every quest in Skyrim, Update, Dawnguard, HearthFires, and Dragonborn. Completion requires evidence per quest and important branch; it is not guaranteed by adding synchronization primitives. The local owned files contain 2,274 distinct QUST records including internal helper quests. **Zero quests have been certified for this fork.** Creation Club and arbitrary quest mods are separate compatibility packs.

The installed Skyrim runtime is 1.7.104. Upstream Skyrim Platform documentation lists older runtimes through 1.6.1170. Native plugin compatibility with 1.7.104 must be established before an in-game test; the existing game installation has not been modified. Building a client bundle is not evidence of engine compatibility.

## Build and verify

Linux build prerequisites: Docker Compose, this checkout with its vcpkg submodule initialized, and legally owned game masters. The pinned builder image supplies cached dependencies. Give the host user ownership of the checkout and private state directory. For a fast first build, copy `/home/skymp/.cache/vcpkg/archives` from that image into `.vcpkg-cache` (the image's default user differs from boyhouse's user).

```sh
export BOYHOUSE_GAME_DATA=/home/andrew/skymp-data
export BOYHOUSE_STATE=/home/andrew/skymp-staging
mkdir -p "$BOYHOUSE_STATE"
chmod 700 "$BOYHOUSE_STATE"
docker compose -f misc/boyhouse/compose.yaml run --rm build
docker compose -f misc/boyhouse/compose.yaml run --rm verify
docker compose -f misc/boyhouse/compose.yaml run --rm init
docker compose -f misc/boyhouse/compose.yaml up -d staging
docker compose -f misc/boyhouse/compose.yaml logs --tail=50 staging
```

Staging binds UDP 17777 and HTTP 17778 to host loopback only. Offline development authentication is unsuitable for public exposure. The five masters mount read-only outside the public `data` directory. No game assets belong in this repository or a client download. Saves/settings/logs live in the private state directory, separate from source. Stop staging with `docker compose -f misc/boyhouse/compose.yaml stop staging`; back up the state directory while stopped before migrating saves. Initialization preserves existing settings.

Pure checks (Node 24 for native TypeScript stripping):

```sh
node --test misc/boyhouse/partyInstances.test.cjs misc/boyhouse/instanceCommand.test.cjs
python -m unittest discover -s misc/boyhouse -p 'test_*.py'
python misc/boyhouse/quest_catalog.py --help
```

The native smoke test loads the five masters and connects party decisions to the compiled addon; it is not an in-game client test. The C++ `[Instance]` tests exercise replication and rejected cross-instance actions. Run the broader native unit executable separately to detect upstream fixture incompatibilities.

## Development client commands

Once a compatible fork client is installed, open the Skyrim console and use `mp "bh status"`. Other commands: `mp "bh invite 0xff000002"`, `mp "bh accept 0xff000001"`, `mp "bh enter boundary_test"`, `mp "bh enter boundary_test 0xff000001"`, `mp "bh kick 0xff000002"`, `mp "bh leave"`, and `mp "bh exit"`. IDs are server character IDs, not arbitrary local reference IDs. Responses print to the console; `status` includes your server character ID. Entry requires being inside the registered boundary; exit requires being outside. Party membership alone does not move anyone. These commands are a diagnostic bridge, not automatic location phasing.

## Next acceptance gates

1. Confirm native Windows support for the installed runtime and pass a three-client in-game disappear/reappear, party entry, kick, reconnect, and delayed-packet test.
2. Add transition epochs, reference identity `(campaign, original form)`, per-campaign vanilla overlays, and world-state transactions around the implemented personal journal; exercise these systems across the five-master quest catalog.
3. Implement quest VM/API, dialogue/scene/alias and story-manager coverage; validate every scoped quest and branch with solo, party, divergent-history, restart and late-join runs.
4. Expand the evidence-backed quest matrix to all scoped content, then pass 16/32-client load and recovery tests. Never convert an inventory entry to supported without recorded evidence.

## Windows runtime compatibility investigation

The client TypeScript bundle builds using the upstream Yarn lockfile. Windows CMake configuration and all 60 native dependency installations also completed. The native plugin uses a 2024 CommonLib pin whose address database reader rejects unexpected format versions, and `Hooks.cpp` explicitly looks for `skse64_1_6_1170.dll`. These are concrete porting targets, not proof that every hook will work after an upgrade.

SKSE's official site supplies 2.3.1 for 1.7.104: https://skse.silverlock.org/. A current CommonLib candidate is https://github.com/alandtse/CommonLibSSE-NG; migration must preserve SkyMP's four local access/layout patches and audit runtime-dependent hooks. A similar format-5 failure is documented at https://github.com/maglarnet/Dynamic_Terrain_Deformation_Skse/issues/4. Do not install the current build into a 1.7.104 game and label it compatible based only on compilation.
