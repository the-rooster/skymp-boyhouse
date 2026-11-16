# Native client runtime audit

Updated 2026-11-16 UTC. **Experimental port; multiplayer gameplay is unverified.**
The installed Steam runtime is 1.7.104.0. Vanilla Skyrim and a locally built SKSE
2.3.1 both reached the main menu. SKSE logged `init complete` and its data-loaded
message. No personal save was loaded. The SkyMP implementation DLL has not run.

## Implemented port

| Area | Change | Evidence and remaining limit |
| --- | --- | --- |
| CommonLib | Pin [Zzyxz/CommonLibSSE-NG d4ee4110](https://github.com/Zzyxz/CommonLibSSE-NG/tree/d4ee41104c391ac4e19093c97c3a76138bf6b55a), archive SHA-512 in the portfile; rebase four access/layout patches. | Windows Release builds pass for `skyrim_platform`, `skyrim_platform_entry`, and `SkyrimPlatformCEF`. Runtime layouts still need exercise. |
| Plugin metadata | Explicit Address Library V5 and post-629 structure flags. | Describes the dependency format; does not certify gameplay or future runtimes. |
| Entry | Reject unsupported runtime before loading the implementation DLL; on 1.7.104 require inspected Steam PE timestamp/image size; require the matching Address Library file. | Important because implementation static initializers resolve relocation IDs. Entry allows 1.6.1170 and experimental 1.7.104 only. |
| Native discovery | Preserve `BindNativeMethod`'s bool return, skip non-native functions, resolve the SKSE module name from the runtime. | Source audit: VM vtable slot `0x18`; SKSE callback `0x50`, long-signature flag `0x58`. Bridge calls remain untested in-game. |
| SkyrimVM | Use `GetRuntimeData().impl`. | Compiles with the 1.7-aware accessor; native callback execution remains open. |
| Console print hook | Check CALL opcode and exact 1.7.104 site/displacement before patching. | Owned PE disassembly: ID 51110 at RVA `0x90F1E0`, site `+0x300`, bytes `E8 FB A2 83 FF`, target `0x1497E0`. Other hooks still require audit. |
| Support scripts | Reproducible local SKSE/TESModPlatform compilation with strict output parsing. | 63 Skyrim PEX files built and parsed; five focused builder tests pass. Gameplay is not established by parsing. |

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
was switched to 1280x720 windowed mode for testing; the original INI is backed up.
Automated menu inputs did not produce reliable game input, including in vanilla.
This is a test-control limitation, not evidence of a multiplayer regression.

## Open launch prerequisite

The complete [official Address Library v13](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)
for 1.7.104 is still missing. Both available browser profiles require a Nexus
sign-in to download it. No account or credential changes were attempted.

The author's public `AddressLibraryDatabase` was inspected: its current `.relib`
has older runtimes through 1.6.1179, not 1.7.104. A separately published MIT table
from [marthofdoom/CommonLibSSE-NG](https://github.com/marthofdoom/CommonLibSSE-NG/tree/961e880a4d40dec94050014ea2b4f4a4bb4634ad)
matches the owned PE identity, but lacks 11 of 28 literal AE IDs in SkyMP's platform
sources. It was used for read-only disassembly research; it was **not** installed,
converted into an official Address Library, or used to fill gaps with guesses.

## Remaining acceptance

Obtain the correct Address Library, finish hook/layout checks, package the native
and TypeScript client, and exercise native calls and event delivery in-game.
Then connect two and three real clients and test movement, party entry/exit,
disappearing/reappearing players, stale messages, reconnects, native journal
projection, and restoration of personal state. World overlays and general quest
execution remain unfinished. A community release additionally requires the
16/32-client load and recovery suite and recorded quest/branch playthroughs.
