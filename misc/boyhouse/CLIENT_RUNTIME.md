# Native client runtime audit

Updated 2026-11-16 UTC. **Experimental port; multiplayer gameplay is unverified.**
The installed Steam runtime is 1.7.104.0. The locally built SKSE 2.3.1 and
Skyrim Platform implementation now load a generated test world and execute native
JavaScript/Papyrus bridge calls. A clean restart reproduced this result. No
personal save was loaded or changed. The full multiplayer client is not installed,
and no real game client has connected to the server yet.

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

This test did not validate movement input or multiplayer. Automated menu clicks
still did not dismiss the Survival Mode prompt. The observed load order also
includes Fishing and Saints & Seducers masters plus ESL content beyond the five
server masters; content/load-order alignment is required before multiplayer.

## Remaining acceptance

The packet-epoch transport adds a separate `MpClientPlugin` codec version 2.1.0.
Its binary frame retains the inner packet and exposes decoded frames to the
TypeScript networking service. Epoch-aware sessions refuse earlier codecs with
gameplay input closed. Native unit tests cover frame round trips, stale-epoch
rejection and reconnecting inside a packet callback; service tests cover every
outgoing path and bounded replication replay. This transport is not yet enabled
by the projection coordinator and does not validate Skyrim's hooks or layouts.

Finish hook/layout checks, align content, and package the native and TypeScript
client. Broaden native API/event coverage beyond the smoke above.
Then connect two and three real clients and test movement, party entry/exit,
disappearing/reappearing players, stale messages, reconnects, native journal
projection, and restoration of personal state. World overlays and general quest
execution remain unfinished. A community release additionally requires the
16/32-client load and recovery suite and recorded quest/branch playthroughs.
