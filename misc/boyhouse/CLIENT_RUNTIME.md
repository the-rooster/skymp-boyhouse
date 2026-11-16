# Native client runtime audit

Inspected 2026-11-16 UTC. The installed Steam executable reports **1.7.104.0**. No SKSE installation or in-game execution has been performed. The previous successful Windows build does not establish compatibility with this executable.

## Findings in this checkout

| Area | Current implementation | Required validation/change |
| --- | --- | --- |
| CommonLib | `overlay_ports/commonlibsse-ng-flatrim/portfile.cmake` pins CharmedBaryon `b93280e8` from 2024, plus four local patches. | Select and audit a 1.7-capable dependency; rebase the four patches; pin its commit and archive hash. |
| Native method discovery | `Hooks.cpp` names `skse64_1_6_1170.dll`, reads native-function fields at `0x50`/`0x58`, and hooks VM vtable slot `0x18`. | Resolve the loaded SKSE module and verify the native-function layout and virtual slot against the target runtime. |
| Papyrus bridge | `PapyrusTESModPlatform.cpp` directly reads `SkyrimVM::impl`. | Use verified runtime accessors; test native calls and the update callback on 1.7. |
| Hook sites | `game/Offsets.h` combines relocation IDs with fixed instruction offsets, including the console-print call site. | Verify target instructions and signatures before writing hooks. Address Library IDs alone do not validate offsets within functions. |
| Entry/load order | The entry DLL loads `SkyrimPlatformImpl.dll`; its implementation has static relocation objects and installs several hooks. | Compatibility checks must precede dependent initialization, not merely run after static relocations have already been resolved. |
| Quest projection | The TypeScript replica exposes a diagnostic journal and never calls local `Quest.SetStage`. | Native journal/objective projection and phase reset/rebinding must avoid client fragment execution. |

## Upstream leads, not validated replacements

[SKSE's official page](https://skse.silverlock.org/) lists SKSE 2.3.1 for Skyrim 1.7.104. Its current AE download is linked through Nexus. The matching loader and Address Library still need to be obtained and tested locally.

[alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG/tree/ng) and [Zzyxz/CommonLibSSE-NG](https://github.com/Zzyxz/CommonLibSSE-NG) are candidate codebases to inspect. The latter documents 1.7 changes to SkyrimVM, PlayerCharacter, input devices/handlers and Address Library format 5, as well as unverified layouts and missing IDs. Those maintainer claims are leads for a port, not evidence that this SkyMP client works. Replacing the dependency alone is insufficient.

Acceptance requires launching the native client, exercising its Papyrus bridge, connecting two then three clients, and testing movement, party entry/exit, disappearing/reappearing players, stale packets, reconnects and quest projection. A community release additionally needs 16/32-client load and recovery testing.
