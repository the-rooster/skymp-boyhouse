# Native client runtime audit

Updated 2026-11-16 UTC. **Experimental port; full multiplayer gameplay is unverified.**
The installed Steam runtime is 1.7.104.0. The locally built SKSE 2.3.1 and
Skyrim Platform implementation now load a generated test world and execute native
JavaScript/Papyrus bridge calls. A clean restart reproduced this result. No
personal save was loaded or changed. The full multiplayer client has now logged
into the private staging server and loaded its generated character. The
single-client shared/private/shared projection round trip described below has
passed on a disposable server. New-character completion, multiple real players,
general movement/world behavior and quest playthroughs remain unverified.

## Implemented port

| Area | Change | Evidence and remaining limit |
| --- | --- | --- |
| CommonLib | Pin [Zzyxz/CommonLibSSE-NG d4ee4110](https://github.com/Zzyxz/CommonLibSSE-NG/tree/d4ee41104c391ac4e19093c97c3a76138bf6b55a), archive SHA-512 in the portfile; rebase four access/layout patches. | Windows Release builds pass for `skyrim_platform`, `skyrim_platform_entry`, and `SkyrimPlatformCEF`. Runtime layouts still need exercise. |
| Plugin metadata | Explicit Address Library V5 and post-629 structure flags. | Describes the dependency format; does not certify gameplay or future runtimes. |
| Entry | Reject unsupported runtime before loading the implementation DLL; on 1.7.104 require inspected Steam PE timestamp/image size; require the matching Address Library file. | Important because implementation static initializers resolve relocation IDs. Entry allows 1.6.1170 and experimental 1.7.104 only. |
| Native discovery | Preserve `BindNativeMethod`'s bool return, skip non-native functions, resolve the SKSE module name from the runtime. | Source audit: VM vtable slot `0x18`; SKSE callback `0x50`, long-signature flag `0x58`. The native smoke below exercises string, numeric and form arguments/results, plus SKSE discovery. Long signatures and general API coverage remain open. |
| SkyrimVM | Use `GetRuntimeData().impl`. | The generated world delivers update/load events and a latent game-version callback. General script execution remains unverified. |
| Console print hook | Check CALL opcode and exact 1.7.104 site/displacement before patching. | Owned PE disassembly: ID 51110 at RVA `0x90F1E0`, site `+0x300`, bytes `E8 FB A2 83 FF`, target `0x1497E0`. The native smoke captures its printed marker through `consoleMessage`. Other hooks still require audit. |
| Support scripts | Reproducible local SKSE/TESModPlatform compilation with strict output parsing. | 63 Skyrim PEX files built and parsed; five focused builder tests pass. Gameplay is not established by parsing. |
| Optional fonts | Enumerate with error codes and catch individual SpriteFont load failures. | A missing Fonts directory previously threw inside the render hook and terminated startup. Fresh launches now reach the test world with either that directory absent or a deliberately invalid font; both failures are logged. |

Inspected executable SHA-256:
`846efccf0c1374d71f892907f46549560f2fcb0a75cb87a3eed438baa0f1402f`.
PE timestamp `0x6A8C7046`, SizeOfImage `0x03929000`.

## Local prerequisites and reproducibility

[Official SKSE source](https://github.com/ianpatt/skse64/tree/25b72352adb6543fa6d0bd3795780672b2e238e0)
was built locally at `25b72352adb6543fa6d0bd3795780672b2e238e0`. This is the
post-release source revision with the delayed UI/ModEvent fix; its version header
still reports 2.3.1 for runtime 1.7.104. It is not a downloaded release artifact.
The loader, Steam runtime DLL, Steam loader DLL, and 62 compiled scripts were
installed for the baseline test. No SKSE binaries or scripts are added to Git.

The script builder takes local source and compiler paths and creates a **new**
staging directory. It neither downloads nor installs anything:

```powershell
python misc/boyhouse/build_skse_scripts.py `
  --source build/client-runtime/skse-src `
  --compiler build/client-runtime/caprica/Caprica.exe `
  --output build/client-runtime/verified-scripts-v2 `
  --platform-script skyrim-platform/src/platform_se/psc/TESModPlatform.psc
python -m unittest discover -s misc/boyhouse -p test_build_skse_scripts.py
```

[Caprica v0.3.0](https://github.com/Orvid/Caprica/releases/tag/v0.3.0), source
`2042c902ec269e33c1061ccd8aac0760c981253b`, needs three compatibility treatments:
explicit Native declarations for compiler checking, two void Armor wrappers
written as call-then-return, and a correction to `PexWriter.h::endObject`'s
host-endian, exclusive object size. The size correction is allowed only for
the pinned compiler binary SHA-256 and the exact observed defect. Every output
must then pass the full strict Skyrim PEX parser. Unknown corruption is refused.
Inputs, compiler, flags and outputs are hashed in `manifest.json`; a failed build
has no manifest. The first unvalidated script build was replaced with these
validated outputs before any save was loaded.

Game saves, INIs, and local load-order files were backed up under ignored
`artifacts/game-backups/20261116T084838Z/`. Its `skse-install/manifest.json` records
every installed path/hash and whether an original file was backed up. The game
was switched to 1280x720 windowed mode for testing; the original INI was restored
after the test and its hash matches the backup. Skyrim is closed.
Automated menu inputs did not produce reliable game input, including in vanilla.
This is a test-control limitation, not evidence of a multiplayer regression.

## Address Library and native world smoke

The user downloaded [official Address Library v13](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files).
The archive passed CRC validation. Only its matching
`SKSE/Plugins/versionlib-1-7-104-0.bin` was installed. SHA-256:
`8aab3dd251d135b849bd983f86a4a205c920fa3e81f8e30c0e63ccfef9423842`.
Its V5 header identifies SkyrimSE.exe 1.7.104.0, 8-byte pointers, and 565,759 RVA
entries. All 28 literal AE relocation IDs used by the platform have nonzero,
in-image entries. This checks presence/bounds, not every hook or runtime layout.
The local install manifest is under ignored
`artifacts/game-backups/20261116T155738Z/address-library/`.

The author's public `AddressLibraryDatabase` was inspected: its current `.relib`
has older runtimes through 1.6.1179, not 1.7.104. A separately published MIT table
from [marthofdoom/CommonLibSSE-NG](https://github.com/marthofdoom/CommonLibSSE-NG/tree/961e880a4d40dec94050014ea2b4f4a4bb4634ad)
matches the owned PE identity, but lacks 11 of 28 literal AE IDs in SkyMP's platform
sources. It was used for read-only disassembly research; it was **not** installed,
converted into an official Address Library, or used to fill gaps with guesses.

Launch SKSE with the game directory as its working directory. The loader checks
the Address Library through a relative path; launching from another directory
produced a misleading missing-library warning despite the installed file.

[`runtime-smoke.js`](runtime-smoke.js) is an opt-in diagnostic, excluded from the
release client. Install it only in a dedicated `PluginFolders` directory. Beside
it, `boyhouse-runtime-smoke-settings.txt` containing
`{"loadTemporaryWorld":true}` allows the standard platform `loadGame` helper to
generate a temporary test character at world 60, position
`[133857,-61130,14662]`. Without this flag it does not initiate a save load. The
diagnostic uses the loader's `skyrimPlatform` global, not a Node module import.

The clean restart recorded:

- Platform 2.9.0 and native packet codec 2.1.0; update ticks through 1,800.
- Generated world rendered; `loadGame` and `postLoadGame` events delivered.
- Player form 20, world form 60, expected nearby position, and SKSE `[2,3,1]`.
- `StringUtil.GetLength("boyhouse") == 8`, `Math.Pow(2,10) == 1024`, and
  `Game.GetFormFromFile(0x3c,"Skyrim.esm")` returned form 60.
- Latent `Debug.GetVersionNumber` returned `1.7.104.0` and the console hook
  delivered the diagnostic marker.
- An invalid optional font was logged and skipped without stopping startup.

Windows Release builds passed for `MpClientPlugin`, `skyrim_platform`,
`skyrim_platform_entry`, and `SkyrimPlatformCEF`; the font change was then rebuilt
and tested in-game. The tested implementation DLL SHA-256 is
`936f428e871d15d8dbe32ca032d12188362984329749e988377bf239e2678409`.
Fresh evidence is in ignored `artifacts/runtime-clean-world-smoke/`; installation
and restoration hashes are in
`artifacts/game-backups/20261116T160440Z/platform-smoke/manifest.json`.
All four preexisting personal save hashes stayed unchanged. After the test,
Skyrim was closed through the platform quit API, the diagnostic script/settings
and invalid font were removed, and original display preferences were restored
byte-for-byte. Native dependencies remain installed. No game assets, binaries,
Address Library files, personal saves, or crash dumps are committed.

This diagnostic did not validate movement input or multiplayer. Automated menu clicks
still did not dismiss the Survival Mode prompt. The observed load order also
includes Fishing and Saints & Seducers masters plus ESL content beyond the five
server masters; content/load-order alignment is required before multiplayer.

## Full client connection and startup fixes

The locally built full client logged into the frozen `a28e633` staging server,
received its self `CreateActor`, and loaded world 60. Server logs independently
record the offline test profile login. The five installed master hashes match
`content-baseline.json`. A temporary loopback UDP-over-SSH transport enabled this
test without changing the server's loopback-only port publication. It is a
diagnostic transport, not a latency or performance benchmark. Browser rendering
remained disabled. Character creation opened, but automated menu input did not
finish it; movement and interaction are not established by this result.

The first connection exposed these defects:

- Private servers need their own manifest endpoint. Set the client's optional
  `server-http-url` to the server HTTP origin (for this forwarded test,
  `http://127.0.0.1:17778`). It then requests `/manifest.json`; existing master
  API routing remains the default. The direct request succeeded in-game.
- Inventory setup briefly exposed negative removal deltas to the unsigned
  equipment packet codec. The client now retains the pending equipment change
  until counts are serializable, and retries after a native send exception.
  It does not remove entries from the reported equipment to hide the failure.
- HTTP startup crashed inside `BS::thread_pool<0>::create_threads`. The dump and
  matching PDB located the fault in the thread pool's array replacement.
  `platform_lib` compiles as C++20, while CommonLib consumers compile as C++23;
  the header selects different `move_only_function` types and object layouts.
  `ThreadPoolWrapper` now hides the library behind an implementation compiled
  in one translation unit. The previously crashing reconnect subsequently
  completed the HTTP request and world load.
- `getContainer(baseFormId)` mistakenly required a `TESObjectREFR`. NPC/container
  base records are different types, so that lookup discarded their inventory.
  It now resolves the form first, then checks its `TESContainer` component.
  In-game reads matched eight entries from an owned container record and one
  entry from an owned NPC record; both previously resolved to an empty array.

The mixed-standard regression explicitly links C++20 and C++23 consumers, runs
1,500 tasks with concurrent lazy initialization, checks task exception delivery,
and destroys the pools. Run it independently of the server unit executable:

```powershell
cmake --build build --config Release --target platform_thread_pool_smoke
./build/skyrim-platform/_platform_lib/Release/platform_thread_pool_smoke.exe
node --test misc/boyhouse/clientStartup.test.cjs
```

The full Node regression set passes 63 tests, and the client webpack build and
native Release build pass. The native pool regression also passes. Evidence and
the private crash dump are under ignored `artifacts/full-client-*` directories.
The final implementation DLL SHA-256 is
`8aea716775f3bbbc819fbdac4f477a87edd1700268f6dbeebfa0c5875f25e89f`.
The full-client test manifest is under
`artifacts/game-backups/20261116T163507Z/full-client-test/`. After testing, the
full client/observer plugins were removed, Skyrim was closed, the SSH transport
ended, and both INIs were restored byte-for-byte. That restoration also reversed
the existing client's forced difficulty setting of 5. All four personal save
hashes remained unchanged, and no generated test saves remained. Native fixes
and prerequisites stay installed. Background testing used a backed-up temporary
`bAlwaysActive=1` setting and did not require focus after that change.

That run also exposed starter gear being applied before character creation
finished; the one-time helmet unequip raced with inventory application. See the
loadout fix below. None of these checks certify any quest.

## Character creation loadout

`PlayerLoadoutService` now owns local player loadout application. It keeps the
latest server snapshots queued while the spawn is loading or character creation
is pending/open, unequips the preview before opening the menu, and waits until
the native menu has actually been observed open and then closed before restoring
the loadout. The old independent initial/retry/periodic inventory callbacks no
longer apply equipment behind the preview. A generation token prevents an old
spawn's readiness callback from releasing a later character's inventory.

`SendInputsService` submits the finished appearance before restoration, retains
equipment changes until restoration succeeds, and suppresses preview equip
events so they cannot overwrite saved equipment or trigger item scripts. Normal
play equipment echoes update the cached snapshot without rebuilding the local
inventory. Resetting a connection or loading a personal save discards pending
loadouts; a plugin hot reload preserves a preview already in progress.

The client build and 76 Node tests pass, including delayed/duplicate messages,
blocked native applications, reconnects, hot reload, reentrant resets, and final
appearance/equipment ordering. In a background full-client run against the
unchanged private staging server, five samples over 25 seconds of the actual
RaceSex Menu reported no worn armor or equipped weapons. The generated save
initially contained the old starter armor, so the check exercised its removal.
Live menu completion/restoration remains unverified; that transition is covered
by the automated service tests only.

Evidence is in ignored `artifacts/character-creation-runtime/`; file and save
hashes are recorded in
`artifacts/game-backups/20261116T171823Z/character-creation/manifest.json`.
The test game and transport exited, diagnostic/client plugins were removed,
both game INIs and the platform INI were restored byte-for-byte, and all personal
saves remained unchanged. The server deployment and quest certification status
are unchanged.

## Reversible reference suppression

`ReferenceSuppressionService` now owns a bounded ledger of the original disabled
and activation-blocked flags, with independent owner claims for each reference.
`WorldCleanerService` uses it instead of deleting vanilla actors, killing the
problematic chickens, changing alpha, or moving actors in dialogue. Discovery
runs only while connected in multiplayer. Authored corpses remain visible;
protected server views are excluded. Disconnect releases claims, and a new save
discards the previous world's ledger instead of applying restoration to the new
save's matching FormIDs.

Native disable/enable calls are serialized per reference. Completion must be
observed before settling; a release/reacquire cannot race an earlier operation.
Overlapping owners retain suppression until the last claim leaves. Stored state
survives plugin reloads, while generation checks prevent old completions from
changing a new world's ledger. Dialogue delays hiding without moving or killing
the speaker. Failed native operations remain unsettled and report an error.
Updates reconcile at most 16 references by default (hard maximum 64); the ledger
accepts at most 16,384 references and does not silently discard restoration data.

All 108 Node regressions and the client build pass. The 25 suppression tests cover
native failure/no-op behavior, overlapping ownership, canceled transitions,
reloads, new saves, and the actual cleaner/service integration. A background
native fixture on runtime 1.7.104.0 also passed against an authored ambient
creature: it observed disabled/blocked state, retention by a second owner,
restoration of both original flags, and restoration after cancellation during
an in-flight native hide. The actor remained alive and undeleted, with unchanged
health. This is a flag-restoration check: native AI/enable behavior changed its
position, so it does not prove restoration of transforms or AI state.

Build that isolated diagnostic plugin with:

```sh
node misc/boyhouse/build-reference-smoke.cjs <output.js>
```

It embeds the actual ledger and native adapter, loads a generated world, records
the result, and exits. Installation and INI/save backups are separate. The fixture
uses the SP3 global API and reacquires native wrappers within each callback;
retaining an Actor wrapper across frames is invalid in this runtime. Initial
fixture attempts exposed those two harness errors before the final passing run.
Evidence is in ignored `artifacts/reference-suppression-runtime/`; the passing
backup manifest is under
`artifacts/game-backups/20261116T180417Z/reference-suppression/`. After testing,
Skyrim exited, the test plugin and recorded generated saves were removed, all
three INIs were restored byte-for-byte, and the four personal save hashes were
unchanged.

This removes one destructive cleanup path. Other original-reference mutations
in `FormView`, `ModelApplyUtils`, and `ObjectReferenceEx`, restoration of AI and
transforms, dynamic FormID reuse, long-distance reference eviction, and the
client projection coordinator still need work. The staging server's projection
flag remains off; no additional quests are certified by this test.

## Temporary-reference lifetime and spawn cancellation

`OwnedReferenceService` now owns temporary FormView references from capture until
native deletion completes. Ownership combines the engine reference handle
(including its age) with a client world generation. A matching FormID/base is
insufficient: work and cleanup recheck the handle on update, and a new save
discards the old generation. Only temporary references (`0xff000000` and above)
can enter this deletion path. `getReferenceHandle` is a new native API required
by this client build; an older native DLL is rejected at service startup.

Spawn rotation, position, enabling, resurrection/physics, and deletion are
serialized through the owned-reference queue. Native promise completions only
settle records; subsequent work runs on update with a fresh native wrapper.
Retiring a view cancels its queued work and ready callback, waits for unfinished
native work, then deletes it and releases cleaner protection exactly once.
Deletion failures/no-ops remain unsettled instead of falsely acknowledging a
transition. Work is bounded to 16 references per update (maximum 64), 16 queued
operations per reference, and 4,096 owned references. Capacity is checked before
placing another temporary view.

`FormView.destroy()` now clears local identity/readiness immediately, and a late
completion cannot mark its replacement ready. Reused model-array slots replace
the old remote identity. The untracked `formViewFunc1/2` storage hooks have been
removed: the fork's temporary actors all use the tracked native spawning path.
`WorldView` stops rendering disconnected/personal-save sessions, invalidates old
resume timers, and preserves an explicit rendering gate across startup, game
loads, and reconnections until its caller requests resumption. Delayed character
menu snippets also carry the gate generation, so an old request cannot open the
menu or resume rendering after another transition has taken ownership. The
pre-load event invalidates ownership/suppression and suspends views/cleaner work
before waiting for the loaded-world event, rather than keeping old tasks active
through the loading interval.

All 137 Node regressions pass, including 28 new lifecycle/integration tests. The
client webpack build, native Release build, and C++ formatting checks pass. A
background native test on runtime 1.7.104.0 spawned an authored creature's
temporary copy, observed it ready/alive with the same engine handle, retired it,
observed native deletion/handle invalidation, and repeated with cancellation
during positioning. The canceled copy never signaled ready and was deleted only
after its pending work settled. The fixture embeds the real lifetime service,
queue, spawn process, and object-reference helper:

```sh
node misc/boyhouse/build-lifetime-smoke.cjs <output.js>
```

Evidence is in ignored `artifacts/reference-lifetime-runtime/`; the backup
manifest is under `artifacts/game-backups/20261116T182319Z/reference-lifetime/`.
The installed/tested native DLL SHA-256 is
`7ff5d7c82aa2cbed801f22b8fc5aac3a747469d60221be41014d4746e95cec9b`.
The test game exited, its diagnostic plugin and generated save were removed,
all three INIs were restored byte-for-byte, and all four personal save hashes
were unchanged. The new native API remains installed. C++ formatting after the
native test changed only whitespace, not behavior.

Native appearance/tint spawning, real engine FormID reuse, and an actual plugin
reload during a latent operation are not certified by this fixture. The
live fixture also did not load another save with work pending; load-boundary
coverage here is automated. The
reload/unit tests prove adoption when the outstanding promise settles, but
`SkyrimPlatform.cpp::ClearState` clears completion queues during plugin reload;
a dropped completion can therefore leave cleanup waiting. That needs native
reload coverage/recovery before treating reloads as reliable. Other asynchronous
model/remote-server mutations and original-reference restoration still need
the same lifetime discipline. This is a prerequisite for the phase coordinator,
not a completed engine projection or quest playthrough.

## Observed player-spawn completion

`PlayerSpawnService` now owns the self-create path previously implemented as
independent timers in `RemoteServer`. It waits for the authoritative world/cell,
full three-dimensional position within 128 units, and a loaded player model.
It applies the current appearance, spells and actor values only after stable
position observations, then observes the player on another update before
releasing the matching loadout generation. A nearby player still receives the
authoritative move/rotation. Position retries are bounded to eight attempts,
with a 60-second overall deadline; failure cancels the pending loadout.

A superseding self snapshot, disconnect, actor destruction, manual save load,
or plugin replacement invalidates old spawn work and deferred appearance
callbacks. One generated save can be in flight at a time; a newer snapshot
waits for that load and positions its own player afterward. The loaded event
prevents a second bootstrap even if a network tick precedes the first gameplay
update. Menu events carry loading state into tick context because Papyrus UI
queries are forbidden there. World-view connection resumption likewise starts
its native game-time wait on an update and rechecks the gate generation.

All 158 Node regressions and the client build pass. A full-client connection to
the unchanged private staging server passed on runtime 1.7.104.0: five consecutive
samples observed a connected, loaded, ready player at the server position, with
the character menu open and no worn armor or equipped weapons. The final run
finished in 10.4 seconds after the plugin observer started, with no callback
exceptions in the native log. This tests actual client dispatch/bootstrap and
the character-creation preview; it does not certify play after closing the menu,
phase switching, concurrent players, or quests.

Build the test bundle after building the client:

```sh
node misc/boyhouse/build-player-spawn-smoke.cjs <output.js>
```

The observer is appended to the actual client bundle so it shares that plugin's
storage. It requires isolated offline-server settings and exits the test game
after passing or its deadline. Installation, transport and backups are separate.
Evidence is in ignored `artifacts/player-spawn-runtime/`; the final backup
manifest is `artifacts/game-backups/20261116T185939Z/player-spawn/`. Skyrim exited,
the test bundle/settings were removed, all three INIs were restored byte-for-byte,
and all four personal save hashes were unchanged. Earlier runs exposed the tick
context and repeated-bootstrap bugs above; a fixture log initially also rejected
an undefined native load-event payload. These were corrected before the final run.

This readiness API is a prerequisite for the projection coordinator, which is
still unwired. Rendering/appearance completion beyond the loaded-model check,
native hot reload during outstanding work, the other remote-server asynchronous
mutations, original-reference restoration, and real multi-client quest play
remain open. The staging projection flag and quest certification counts are
unchanged.

## Remote-world replacement and delayed packet effects

`RemoteServer.resetWorldModel()` now invalidates delayed work and replaces the
world model, index manager and hosting list. It cancels the pending player spawn
and emits `worldModelReset`, which retires world views, expires gamemode event
sources, and clears owner/neighbor functions and their model context. Disconnect,
failed/denied connections and personal-save loading use this path. Explicit
transport closure now emits the cleanup notification without redialing.
Generated-world loading preserves its initial packet work but suspends native
application until the matching loaded event.

Remote packet callbacks carry a world generation, and actor-specific callbacks
also retain the identity of the model they target. Reusing an index or replacing
a plugin cannot make an old callback act on the new model. Late ragdoll teleport
completion additionally checks the engine reference handle. Container polling
and animation retries use fresh update callbacks and stop when their request or
reference is replaced. Unknown destroy messages no longer schedule a menu exit;
a newer self model cancels a queued exit. Duplicate index allocation is
idempotent, sparse remote indices use a Map, and upgrading a retained index
manager reconstructs existing model slots without shifting them.

Harvest refreshes now reacquire references on update and verify the original
engine handle and caller generation before enabling them. The ordinary-object
harvest branch no longer deletes authored references. Death recovery delegates
temporary-view deletion to its lifetime owner rather than directly deleting
whatever actor a callback resolves.

All 184 Node regressions pass, including 25 new remote-world/index/harvest/
gamemode cases and an explicit-close transport case. The client build passes.
A full-client startup regression against the unchanged private staging server
passed on runtime 1.7.104.0: connected, correctly positioned and loaded player,
then five unequipped character-menu samples, completing in 11.3 seconds after
the observer started. There were no native callback exceptions. This live test
checks normal connection/reset/bootstrap integration; cancellation, index reuse,
harvest and gamemode replacement are covered by the automated tests, not native
phase-transition certification. The final 100 ms container activation grace and
explicit-close notification were tested and rebuilt after that live run; the
live fixture does not exercise those paths.

Evidence is in ignored `artifacts/remote-world-runtime/`, with backup manifest
`artifacts/game-backups/20261116T191246Z/player-spawn/`. The test game and relay
exited, diagnostic files were removed, all three INIs were restored byte-for-byte,
and the four personal save hashes were unchanged.

This cancels delayed follow-up work; it does not cancel a native operation already
submitted. A phase coordinator still needs to track/drain those operations before
acknowledgement and restore original-reference state. Native ragdoll global
settings, death-service timers, arbitrary trusted gamemode native callbacks,
other FormView mutations and full save/reload recovery need further lifetime
work. The baseline coordinator, native journal projection and quest execution
remain unfinished; staging projection stays disabled and no quests are certified.

## Native operation serialization and cancellation

`NativeReferenceOperationService` now serializes borrowed native work per engine
reference handle. A bounded queue coalesces queued property updates, runs each
sequence without interleaving, and observes native completion before continuing
on an update. Visibility, harvest refresh, motion changes, suppression and
ragdoll recovery use this queue. Superseding a campaign cancels queued work and
late callbacks but keeps an already submitted native call tracked until it
settles. Suppression restoration survives campaign cancellation; loading a new
save invalidates all old work. Failures remain recorded and cannot imply a
successful engine acknowledgement.

The owned-reference lifetime and borrowed-operation queue now coordinate:
borrowed work waits for an active owned native operation, and owned spawn/deletion
waits for pending borrowed work. Thus retiring a view cannot delete its actor
while its visibility/ragdoll work is still running. Every native step resolves a
fresh wrapper and verifies identity; wrappers are never kept across callbacks.

Ragdoll recovery captures the six existing damage difficulty multipliers, shares
the temporary override across overlapping calls, and restores the exact captured
values when the last call settles. It preserves settings changed by another
owner in the meantime. Reload restores a retained override instead of leaving
damage at zero. The former unconditional reset of every multiplier to 1 is gone.
Harvesting no longer activates a random nearby NPC; it changes the harvest flag
and refreshes visibility through the queue, preserving already-disabled plants.

All 203 Node tests and the client build pass. Two scripted native fixtures passed
on runtime 1.7.104.0 using generated saves:

- `build-native-operations-smoke.cjs` exercised actual owned and borrowed client
  services: work waited for owned enable/disable, deletion waited for an entire
  borrowed sequence, epoch cancellation suppressed follow-up work while still
  draining the pending call, and normal/cancelled ragdoll operations restored
  `[0.75, 1.5, 3, 1, 0.5, 2]` exactly. It passed in 6.2 seconds.
- `build-reference-smoke.cjs` now includes the actual native operation service.
  Hide, overlapping suppression claims, restoration and cancellation during a
  pending disable passed in 5.7 seconds. The authored creature retained its
  original alive/health/visibility/activation flags. AI still changed its position;
  this fixture does not prove full reference-state restoration.

The fixture builders only write diagnostic artifacts. Evidence is in ignored
`artifacts/native-operations-runtime/` and
`artifacts/reference-suppression-runtime/`; backup manifests are under
`artifacts/game-backups/20261116T193835Z/native-operations/` and
`artifacts/game-backups/20261116T193920Z/reference-suppression/`. Both test games
exited, all three INIs were restored byte-for-byte, diagnostic files were removed,
and all four personal save hashes were unchanged.

The rebuilt full client also connected to unchanged private staging and passed
the five-sample positioned/loaded/ready and unequipped character-menu observer
in 8.8 seconds. Its evidence is in `artifacts/player-spawn-runtime/`, with backup
manifest `artifacts/game-backups/20261116T193949Z/player-spawn/`. The test game
exited and the same INI/save cleanup checks passed. None of these three native
runs logged callback exceptions. The full-client fixture validates startup and
service registration, not quest play or phase application.

This covers the listed native operations, not all native APIs. Native plugin
reload can still drop an outstanding promise callback; the queue must not be
treated as successfully drained in that case. Original inventory/lock/motion
restoration, remaining death-service timers, baseline application, phase-ready
acknowledgement, quest execution and real multi-client play remain unfinished.
Harvest behavior has automated coverage but has not been certified on native
flora. Staging projection remains disabled and **zero quests are certified**.

## Observed remote spawns and initially dead actors

The client no longer skips a remote actor because its initial model is dead.
`SpawnProcess` accepts the desired death state: it silently kills a living copy
for a corpse baseline and resurrects only when an alive baseline requires it.
`FormView` does not apply its living-actor health/deferred-kill override to a
corpse. If death state changes during spawn, the old owned reference retires and
the replacement receives the new state. Reliable death properties also update
the model's movement hint, and subsequent movement packets cannot contradict an
explicit authoritative death state. Previously the synthetic initial movement
always claimed the actor was alive.

Native-call completion alone no longer makes a spawn ready. The owned spawn
temporarily holds the copy's AI, enables it, waits for its 3D model, applies
death/physics state, and reapplies position to the loaded reference. It requires
two update observations of enabled/loaded state, matching alive/dead state, and
position within 128 units of the target. It then restores and verifies the exact
original AI setting. A failed call, no-op enable/AI restoration, or a 30-second
observation deadline retires the copy without signaling ready. All steps and
cleanup remain inside the owned-reference lifetime queue.

`FormView`, `FormViewArray`, and `WorldView` expose `getSpawnReadiness()` for the
future projection coordinator. It checks model identity, native handle, loaded
state, death state, and outstanding owned/borrowed work; aggregation reports
pending/failed forms and excludes other worlds explicitly. World readiness also
requires the selected self world, current view, rendering gate, and connection.
This is spawn readiness only: it does not certify inventory, equipment, authored
reference restoration, journal state, or quest execution.

All 218 Node tests and the client build pass. Fifteen additional cases cover
corpse creation, changing death state, unloaded/misplaced copies, deadlines,
cancellation, AI restoration, model identity, and readiness aggregation/gates.
The actual native lifetime/spawn fixture on runtime 1.7.104.0 passed living actor,
corpse and ordinary-object creation; both actors restored their original enabled
AI; all three loaded references were deleted; a cancelled spawn never reported
ready. The final run completed in 6.0 seconds without native callback exceptions.
It uses the actual spawn/lifetime classes; the FormView/WorldView aggregation and
packet-model changes have automated coverage, not full live phase certification.

The first two native runs timed out: a newly enabled creature was already moving
away before placement could be observed. Holding AI and reapplying the target
after loading resolved this. Evidence from the final run is in ignored
`artifacts/reference-lifetime-runtime/`, with backup manifest
`artifacts/game-backups/20261116T195424Z/reference-lifetime/`. Each test game exited,
all three INIs were restored byte-for-byte, diagnostic files were removed, and
the four personal save hashes stayed unchanged. The final AI-restoration no-op
guard was unit-tested and rebuilt after the live run; that native fixture had
independently checked the restored AI flags.

At this milestone the projection coordinator was still unwired. The following
section records its integration; staging projection remains off.
An actor whose engine death state cannot be changed fails rather than becoming
an acknowledged incorrect copy; essential/protected actor-base overrides still
need campaign support. Corpse inventory, general reference restoration, remaining
combat/death lifecycle work, and quest runtime integration are not completed by
this change. **Zero quests are certified playable.**

## Experimental world projection coordinator

`CampaignProjectionService` connects the version-2 protocol to
`NativeWorldProjectionService` when `campaign-projection: true` is configured.
Transitions block gameplay input, retire the previous model and owned copies,
drain outstanding native calls, and restore previously suppressed originals.
Only then does the normal networking dispatcher apply the validated baseline.
The self baseline can bootstrap a generated save before update events exist.

Once the player is observed ready, the adapter captures and suppresses original
references in batches of 16, installs original-to-copy aliases, and enables
remote rendering. A mapped original never becomes a fallback interaction target
while its copy is missing. Ordinary virtual NPC IDs receive suppression too.
The coordinator waits for original baseline application, suppression, temporary
copy cleanup, and spawn readiness before acknowledgement. Following server
release, it applies queued replication and observes the resulting native work
before restoring the controls it changed. Previously disabled controls remain
disabled; camera and looking controls remain available during a transition.

The journal clears during each transition and subscribes again after release,
so a subscription cannot be dropped by the input gate or retain another phase's
history. Connection generations reject stale acknowledgements, old engine
callbacks, and delayed failure closures. Transitions have a 30-second deadline;
receiving replacement manifests does not extend it indefinitely. Failure keeps
input closed and schedules native network cleanup outside its packet callback.

The full client builds and 243 Node tests pass. These cover the actual service
bindings and native adapter with controlled engine responses, including queued
replication, reconnects, native no-ops, identity mismatches, missing originals,
control restoration, and phase-specific journal subscriptions. This is not
end-to-end quest certification.

The first disposable native server run completed the initial shared-world
handshake, positioned the player, and restored controls. Instance entry exposed
a server script-inheritance error: allocated NPCs had persisted leveled template
choices, but script loading still walked the original leveled list. Script
loading now consumes the validated copy's saved choice. The native instance,
quest, and SQLite selection passes 159 cases with 90,414 assertions, including
that regression; the native projection bot protocol test also passes.

The full client then passed a real shared-world → personal-instance →
shared-world round trip on Skyrim 1.7.104.0 in 12.484 seconds. The fixture used
an existing offline test character and one authored NPC selected by the native
area scan. It observed the original hidden/activation-blocked, the replicated
copy loaded with the correct base and alias, and controls restored after entry.
Incremental native movement went through ordinary client movement replication
and server boundary reconciliation. After exit, five observations verified the
original visibility/activation/alive/health state, deleted temporary-copy
identity, cleared aliases, and restored controls. The final native queues,
owned-copy set, and suppression ledger were empty. This proves that specific
transition, not quest execution, arbitrary locations, or multiple real players.

Fixture development also exposed two assumptions: an explicit exit is refused
inside an altered area, and generated-save loading temporarily blocks native
scripted teleports. The observer now moves across the boundary and retries
against observed position within a bounded window. Player movement/teleport
readiness across that native grace period still needs broader acceptance.

Evidence is in ignored `artifacts/projection-runtime-runtime/`,
`artifacts/projection-runtime-server-e.log`, `artifacts/projection-native-tests.log`,
and `artifacts/projection-template-protocol.log`. The successful run's backup
manifest is `artifacts/game-backups/20261116T204325Z/projection-runtime/`.
Skyrim exited, all three INIs were restored byte-for-byte, diagnostic files were
removed, all four personal save hashes were unchanged, and no new saves remained.
The disposable loopback server and SSH relay stopped. Private staging was not
redeployed.

New-character creation under projection, complete original inventory/lock/
motion restoration, native journal integration, and the full quest runtime
remain unfinished. The adapter's readiness checks cover spawn/lifetime work;
they do not certify every inventory, equipment, animation, or arbitrary
gamemode effect. Staging projection remains disabled and **zero quests are
certified playable**.

## Character creation and visibility refresh guards

`PlayerLoadoutService` now owns the character-creation preview and draft. It
waits for projection readiness before opening RaceSexMenu and keeps the gameplay
input/control lease throughout editing. Closing the menu captures one immutable
appearance snapshot; reliable retries continue until the assigned actor's
matching server echo or same-actor replacement baseline acknowledges it. A
projection refresh preserves the active menu/draft for that same character;
replacing the character during editing is refused. Human editing has no deadline,
while missing menu opening, capture, acknowledgement or restoration fails closed
after 30 seconds of active processing. Projection pauses suspend those deadlines.

The native server separately permits only appearance updates while its assigned
actor's race menu is open (plus the projection control channel). It checks the
actor index and finite appearance values, stores the accepted appearance, then
closes the creation flag and sends the reliable echo. Client outbound gating also
blocks raw and ordinary gameplay messages during creation. Stale generations
cannot close a replacement connection after a loadout failure.

The visibility commit barrier is separate from character creation. Before a
private copy changes enabled state, every connected assigned observer in its
phase is gated and advances its native packet epoch. Fresh projections follow
the committed change; the native gate cannot open inside the barrier or after a
terminal visibility failure. Client hello can defer publication while the gate
remains closed. A failed update quarantines captured connections; reconnecting
into a reused user slot does not inherit another connection's cleanup.

Binary decoding now checks primitive reads, constants and string/container
lengths before accepting values. Truncation and impossible lengths fail instead
of becoming partial appearance or gameplay state. These checks complement the
epoch and actor-identity guards; they are not gameplay certification.

Self loadout restoration now observes normalized full inventory equality on two
updates before handling equipment. It equips existing inventory items without
queuing the older additive equipment-inventory task, then observes worn variants,
hands and spells on two updates before releasing the creation gate. No-op native
calls cannot satisfy that check. Retries are spaced at least 250 ms apart; paused
menus and unloaded actors wait. Ordinary play retains its five-second inventory
checks without taking the creation input lease. The inventory comparer retains
its existing normalization; equipment preserves names and charges through native
extra-data parsing and every merge, then compares explicit names exactly (only
absent/empty names normalize to the base form name). When selection is required,
differing variants of one base ID are
refused before changing gear because `equipItemEx` cannot select a specific item
instance.

Controlled service tests now cover these observations and failed/no-op native
calls; **284 JavaScript tests and the client webpack build passed**, including six
regressions through the real native-extra-data parser. Evidence is
in ignored `artifacts/client-loadout-settlement-tests.log` and
`artifacts/client-loadout-settlement-build.log`. The native attempt below reached
the creator but did not complete it. Actual creation-to-gameplay
behavior, ambiguous item-instance selection, visibility refreshes on multiple
real clients and the full quest lifecycle still require acceptance. A first
`OnLoad` that requests another visibility mutation is refused while its outer
barrier is active. No quest adapters are enabled and **zero quests are certified
playable**.

## Fresh-character native acceptance on 2026-11-16 UTC

A fresh character opened RaceSexMenu after projection reached ready. The
observer recorded **45 seconds of editing across 2,673 samples**, with no worn
gear or equipped weapons and gameplay input still gated. This establishes the
creation preview and its extended editing gate in that run.

The first native `Ui.invokeIntA` call to `...onDoneClicked`, using an empty
argument array, failed in `SP3NativeValueCasts::JsObjectToNativeObject`:
`_skyrimPlatform_indexInPool` was 0, but the valid range was `[34438, 34441)`.
The fixture ended with `passed: false` and `accepted: false` while still editing.
It did not establish menu completion, accepted appearance echo, final loadout
restoration, gameplay release or the planned phase round trip. This binding
failure remains the immediate blocker for completing the scripted creator run;
the automated native/JavaScript/Python suites and server smokes remain green.

Evidence is in ignored
`artifacts/creation-runtime-runtime/boyhouseCharacterCreationRuntimeTest-logs.txt`.
The backup manifest is
`artifacts/game-backups/20261116T220543Z/creation-runtime/`. After the attempt,
all original INIs were restored byte-for-byte, personal save hashes were
unchanged, no new saves remained and Skyrim was closed.

A separate projection rerun never launched. Automatic approval review rejected
the combined preparation/launch action with "blocked by policy" and supplied no
specific reason. Its server and relay were closed; this attempt contributes no
new projection acceptance evidence. Earlier recorded projection evidence is
unchanged. **Zero quests are certified playable.**

## Remaining acceptance

The packet-epoch transport adds a separate `MpClientPlugin` codec version 2.1.0.
Its binary frame retains the inner packet and exposes decoded frames to the
TypeScript networking service. Epoch-aware sessions refuse earlier codecs with
gameplay input closed. Native unit tests cover frame round trips, stale-epoch
rejection and reconnecting inside a packet callback; service tests cover every
outgoing path and bounded replication replay. The experimental server projection
feed now rotates epochs and transfers authoritative world baselines using
protocol version 2; native bot sessions verify that path. The client replica
requires both the baseline and manifest before engine acknowledgement. The
Skyrim-side coordinator is now opt-in, with acceptance limits described above.
See [the protocol contract](QUESTS.md#packet-epochs-and-world-baselines-experimental-server-integration).

Finish hook/layout checks, align content, and package the native and TypeScript
client. Broaden native API/event coverage beyond the smoke above.
Then connect two and three real clients and test movement, party entry/exit,
disappearing/reappearing players, stale messages, reconnects, native journal
projection, and restoration of personal state. World overlays and general quest
execution remain unfinished. A community release additionally requires the
16/32-client load and recovery suite and recorded quest/branch playthroughs.
