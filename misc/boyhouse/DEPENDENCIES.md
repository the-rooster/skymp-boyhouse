# Catalog-wide quest dependency analysis

The analyzer identifies shared implementation work across the five owned masters.
It runs offline on boyhouse; it does not start Skyrim, execute fragments, mutate
campaign saves or enable quest adapters.

The generated [implementation backlog](QUEST_BACKLOG.md) contains aggregate counts.
The full JSON report stays outside the repository at
`/home/andrew/skymp-analysis/quest-dependencies.json`. It provides each quest's
stages/objectives, script roots, traced scripts, cross-quest links, capabilities,
call-site/record evidence, unresolved links and runtime-dispatch candidates.

## Run

Python 3.10+ is sufficient for the current owned, uncompressed script archive.
BSA 104 compressed entries use the standard library; compressed BSA 105 entries
require the optional `lz4.frame` module. Missing support is an explicit error,
never an omitted script.

From the repository root:

~~~sh
python3 misc/boyhouse/quest_dependencies.py \
  --data-dir /home/andrew/skymp-data \
  --archive '/home/andrew/skymp-data/Skyrim - Misc.bsa' \
  --output /home/andrew/skymp-analysis/quest-dependencies.json \
  --markdown /home/andrew/skymp-analysis/quest-backlog.md

python3 -m unittest discover -s misc/boyhouse -p 'test_*.py'
~~~

The existing `verify-server.sh` now runs the full owned-content analysis in the
builder and writes ignored reports under `build/`.

Repeat `--archive` in runtime precedence order. Optional repeated `--scripts-dir`
directories have priority over all archives. The first definition wins, including
a malformed winning script: a lower-priority copy must not mask an error.
The report records script overrides and content hashes. Match the actual runtime's
loose/archive inputs before using a report for a release.

## What is measured

- Winning records are resolved by original master and local FormID, not raw
  load-order numbers. Deleted overrides remain deleted.
- Every QUST is inventoried, including internal helpers. Dialogue topics and
  responses are associated through winning topic ownership and INFO group
  parents. Scene parent quests are distinguished from package-action PNAMs.
  Story-manager quest nodes and their parent conditions are linked.
- QUST, INFO, SCEN and PACK VMAD bindings are decoded, including alias script
  bindings, object properties/arrays and fragment entry points.
- Reference/location aliases, fill strategies, known FormID fields, stage/log
  flags, condition function indices, flags and evaluation contexts are recorded.
  ALCA is an alias/create descriptor, never interpreted as a FormID.
- Original PEX declarations and instruction call sites supply receiver types.
  All directly attached functions/states and custom-parent functions are
  potential roots. Native-bearing parent helpers are traversed when called;
  inherited event handlers are also roots. Calls, parent calls, static calls and
  property handlers are followed with cycle guards and native inheritance.
- Per-capability counts deduplicate quests. Repeated call sites retain evidence.
  Cross-quest data links are listed without recursively attributing the entire
  other campaign to the referring quest. Engine PlayerRef is explicitly marked
  as a required campaign-player binding.
- Source filenames, debug text, literal call arguments, dialogue text and PEX
  bodies are discarded from reports. No owned game assets are checked in.

The capability registry audits **only the isolated campaign fragment runner**.
Native APIs available elsewhere in SkyMP do not imply campaign support.
Logical campaign form and alias getters are now audited subsets; the broader
alias/world feature gates remain missing until filling, effects and lifecycle
are implemented.
Quest-variable conditions now contribute their target-quest dependency, and
global comparisons contribute their GLOB dependency. Explicit Run On Reference
conditions contribute the placed-reference dependency. `GetItemCount`
contributes its base-item dependency; `GetInFaction` and `GetFactionRank`
contribute their FACT dependency unless the parameter uses an unsupported
alias/package flag. Run On Quest Alias reads the signed alias ID at byte 28;
alias zero is valid and the unused byte-24 value is never a FormID.
Comparison globals have
their own audited subset; unsupported flags and non-stage evaluation remain
separate missing capabilities.
Implemented APIs are labeled `implemented-subset`; the report never calls a
quest supported. SHA-256 guards on the audited runtime sources force a review
when the implementation changes. Condition names are identifiers extracted from
[xEdit's definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsTES5.pas);
their source hash is recorded in the registry.

The current registry contains **64 partial native methods**, including ten
physical-reference queries, five private-copy visibility methods and five
faction methods. Queries
require tracked owner/private-copy snapshots; visibility additionally requires
transactional staging and a complete observer projection barrier. The same-named
condition functions are not enabled by these native registrations. A separate
typed stage-log evaluator now audits five physical condition subsets:
`GetDisabled` (35) and `GetItemCount` (47) on explicit references, and `GetDead`
(46), `GetInFaction` (71) and `GetFactionRank` (73) on explicit references or
safely resolved reference aliases. The registry has **eleven partial condition
functions**. Qualified stage-selector features mark
only these combinations as partial; implicit physical subjects, unsupported
alias/function combinations and non-stage evaluation retain missing gates.
Source guards
include the barrier bridge, projection feed, character-creation gate and binary
decoder used by this path. These guards detect source changes; they do not prove
behavior or expand the analyzer's graph coverage. The generated backlog must be
regenerated against the updated registry before its capability counts are used.

Faction reads use distinct rank and membership semantics. Unique actors' existing
faction removal and rank writes below -1 still need isolated base-layer modeling;
raw SKSE `GetFactions` remains unsupported. The client bridge requires observed
native state and restoration, with explicit rank and membership in its transport.
Neither source coverage nor these partial methods certify any quest.

The client native-array bridge is guarded separately from campaign capabilities.
Accepting JavaScript arrays as typed Papyrus native arguments does not implement
any missing isolated-server native API or durable latent continuation.

The [runtime contract](QUESTS.md#physical-reference-queries-and-visibility-mutations)
lists the limits: no fade, enable-parent propagation or latent support; a fresh
`OnLoad` that attempts another visibility mutation is refused while the outer
barrier is active. No adapters are enabled and the certified quest count remains
zero.

## Limits

This is a conservative *potential-dependency* inventory within its documented
scope, not a complete reachability graph or an estimate of playable quests.
Unused functions and alternative branches can contribute dependencies.
Declared receiver types cannot prove actual runtime dispatch. The JSON
separates unresolved static links from sites whose runtime receiver may differ.

The runtime now binds placed-reference scripts, but the dependency analyzer
does not traverse world-reference and asset script bindings yet. Its optional
`plugin_records(..., extra_types=...)` input supports focused reference audits
without silently widening the catalog's claimed analysis scope. Dynamic alias
fills, runtime-created forms, computed IDs, native-engine behavior and external
events require additional discovery and traces. Other non-script record
relationships are also incomplete. These are global analysis gaps even when a
quest has zero unresolved static links. Every active row has
`analysisComplete: false` and `status: untested`.

Malformed plugin boundaries fail the run. Unsupported/malformed individual
VMAD/PEX bindings remain explicit unknowns. Unindexable top-level record IDs are
recorded in each source's `unindexedRecords`; the current files contain one
GMST with an out-of-range master index. It is not guessed into another master.

## Using the backlog

Lifecycle discovery records the exact owned DNAM and INDX flags for repeatable
stages, start-game-enabled quests, run-once quests, startup/shutdown stages and
keep-instance-data stages. Stage flags retain their stage index in evidence.
DNAM must be 12 bytes and INDX four bytes. Only repeatable stage execution has a
registered runtime subset; the other flags describe dependencies, not working
startup, shutdown or reset support. See `QUESTS.md` for the execution limits.

Counts rank impact, not implementation order or effort. Campaign reference
identity and overlays provide a foundation; complete alias filling, lifecycle
and event execution, transactional world effects, condition contexts,
dialogue/scenes and latent continuation persistence remain required.
Existing engine-wide gates such as client projection and phase transitions apply
even to records with few observed script dependencies.

For each capability, implement its semantics once, add behavioral regressions,
review the registry, and rerun the complete catalog. Use runtime traces to extend
the graph and generate further regression fixtures. Actual solo/party/restart
playthroughs and branch evidence remain required before enabling a quest.

Parser references:
- Existing `libespm/QuestScriptData.cpp` and `papyrus-vm/Reader.cpp`.
- [xEdit record definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsTES5.pas).
- [BSA library format reader](https://github.com/Ryan-rsm-McKenzie/bsa/blob/master/src/bsa/tes4.cpp).
