# Campaign quest implementation

This fork targets separate personal campaigns in a shared world, with invited guests using the owner's history inside altered locations. Full coverage means playable quests, scripts, dialogue, rewards, and world changes for the five owned masters. It is **not implemented yet**. The catalog still has **zero certified quests** out of 2,274 QUST records (including internal helpers).

## Implemented journal foundation

- The owning character's existing change form persists a private, versioned quest journal. Keys use native `FormDesc` format, for example `4e50d:Skyrim.esm`; the inventory scanner uses a different display format.
- In shared space, a character reads/writes their own history. Inside `campaign:<owner>:<area>`, an admitted character reads/writes the owner's history. Leaving, being kicked, or recovering from a disconnect restores access to personal history without copying host progress into it.
- Visited stages, highest stage, lifecycle flags, and objective visibility/completion/failure are explicit. Unknown records, nonexistent stage/objective indices, stale revisions, malformed saves, and oversized saves are rejected before the single change-form update.
- Twenty-three Quest methods are registered with the server Papyrus VM. They operate on journal state; they do not yet implement vanilla quest execution. Script contexts capture character lifetime and both character/owner instance epochs. A latent stack cannot write after either participant leaves and re-enters the same instance.
- `GetQuestRunning`, `GetStage`, `GetStageDone`, and `GetQuestCompleted` condition functions use the selected actor's campaign. Their indices follow [TES5Edit's definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsTES5.pas). This supports these predicates in the existing condition evaluator; it does not add a dialogue engine or fix its other unsupported conditions/run-on types.
- Experimental servers publish read-only journal snapshots to subscribed clients. Connection identity selects the actor. A subscription nonce and increasing sequence reject old connection/party snapshots; context epochs permit switching to a different campaign with a lower revision. Errors clear the client projection. Unchanged journals are not resent.
- The forked client subscribes on connection and exposes `mp "bh journal"` for diagnostics. It does **not** invoke local `Quest.SetStage`, which would run unsupervised client fragments and potentially duplicate effects. The native Skyrim quest menu is not integrated.

## Trusted server API

### Transactional stage fragments

`mp.set(actorId, "questExecuteStage", { quest: 0x4e50d, stage: 100, expectedRevision: 7 })` executes an original stage through the isolated runner. This is a trusted gamemode API, not a network command. It requires the same per-campaign adapter opt-in as journal mutations. Configure `archives: ["/game-data/Skyrim - Misc.bsa"]` to supply owned scripts, outside the public data directory. Script storage precedence remains loose scripts, configured archives in order, then bundled assets; a release must pin that content explicitly.

The runner now implements:

- Bounds-checked QUST VMAD versions 4/5, both object layouts, properties/arrays, stage/log-entry bindings, and alias script metadata. Parsed values own their storage. Alias metadata is parsed, but aliases are not instantiated yet.
- Fresh, campaign-scoped quest objects and a strict VM for each transaction. Identical quest form IDs in different campaigns do not share Papyrus identities or variable maps.
- Original PEX loading, including BSA decompression. The reader checks headers, bounds, string-table indices and call argument counts, handles absent debug information, and distinguishes parameters from locals.
- Conditional log-entry selection using the four implemented quest predicates, in the host's context for guests. Unsupported condition functions, run-on contexts, and log flags are rejected. Matching fragments execute in log-entry order; the complete-quest log flag updates the same transaction.
- Primitive variables, shared arrays, script state and quest-object references persisted with the journal. Variables are qualified by attached script and declaring class; references use stable plugin/form keys. Private VM state is omitted from client snapshots. Resetting a quest clears its saved script variables and collects arrays only when no other script references them.
- Nested `SetStage`/`SetCurrentStageID` calls execute target fragments in the same VM, campaign, instruction budget and transaction. Cross-quest calls require the target quest's own adapter opt-in. Visited stages break cycles; invalid targets and failures roll back every affected quest. `Start`, `Stop` and `Reset` execution still require lifecycle/event support.
- One commit for journal changes and script variables. Unsupported natives, missing bindings, stale contexts, invalid property targets, invalid array access, runaway execution, and latent calls throw and discard the transaction. A VM-wide instruction budget also covers nested calls. World/inventory effect natives are deliberately absent from this runner until those effects can participate in the transaction.

Original MQ102 stage 100 and DA04 stage 5 are execution fixtures. The DA04 fragment displays objective 5; a guest's execution changes only the host's journal. MQ102 stage 100 has no visible objective effect from a fresh journal. Its stages 10/30 reach unsupported map-marker operations, stage 50 reaches an alias, and stages 20/200 need nested lifecycle execution. Those operations fail rather than being omitted. These fixtures do not start or play the quests: no initialization events, alias filling, dialogue, scene execution, rewards, or world overlays are supplied. Repeated stages are currently idempotent, even for quests that eventually need repeatable-stage semantics.

VM save schema 2 separates script roots from a campaign-local array heap. Multiple variables/scripts retain one allocation across fragments and character reloads, including mutations through a script whose other sharers are unloaded. Equal-valued allocations remain separate; null and empty arrays remain distinct. Legacy inline arrays migrate on the next successful transaction. Unreachable allocations are collected. Papyrus equality compares array identities and recognizes typed null arrays as None. The runner remains an experimental subset; latent stacks, timers and object lifecycle persistence are unfinished.

### Journal state operations

`mp.get(actorId, "questJournal")` returns:

```js
{
  schemaVersion: 1,
  revision: 7,
  actor: 4278190082,
  campaignOwner: 4278190081,
  instance: "campaign:4278190081:fixture",
  contextEpoch: "1:1",
  quests: {
    "4e50d:Skyrim.esm": {
      stage: 20, stages: [10, 20],
      running: true, started: true, completed: false, active: false,
      objectives: { "10": { displayed: true, completed: false, failed: false } }
    }
  }
}
```

`mp.set(actorId, "questCommand", { quest: 0x4e50d, op: "setStage", index: 20, expectedRevision: 7 })` is a trusted state-only gamemode operation, never a client command. Supported operations are `start`, `stop`, `reset`, `complete`, `setActive`, `setStage`, `setObjectiveDisplayed`, `setObjectiveCompleted`, `setObjectiveFailed`, `completeAllObjectives`, and `failAllObjectives`. Boolean operations take `value`; objective/stage operations take `index`. `expectedRevision` is optional optimistic concurrency protection. Repeated visited stages do not rerun or advance the revision. Use `questExecuteStage` when the adapter intends to execute fragments; writing journal state alone never invokes them.

Mutations require the owner's private `private.boyhouse.questAdapters` array to contain the quest's stable key. This is a trusted adapter opt-in, **not automatic evidence that an adapter is correct**. No production vanilla adapter is supplied or enabled. The fixtures explicitly opt in selected quests to validate plumbing, conditional selection, and an original fragment; they do not play or certify Before the Storm.

The persisted field is `private.boyhouse.questJournal`. Unknown save versions are refused rather than reset. The implementation validates before writing and uses the existing character save path; it does not yet provide atomic transactions spanning quest state, inventory rewards, and world overlays. The ledger limit is 8 MiB. The diagnostic feed currently sends full snapshots, capped at 1 MiB; exceeding that limit returns unavailable rather than truncating progress. Delta/chunk transport is required before full campaign rollout. VM epochs are process-local guards, not network transition acknowledgements or VM persistence.

## Remaining implementation gates

| Gate | Required work and evidence |
| --- | --- |
| Original quest execution | Extend the implemented scoped fragment runner with startup/stop events, alias filling and alias scripts, nested quest lifecycle transitions, repeatable stages, and the remaining condition/native semantics. The existing global VM must not become an alternate campaign-effect path. |
| VM persistence | Variables now persist; add aliases, timers, latent continuations, content hashes and migrations. Restart during a stage/reward must neither lose nor duplicate effects. |
| World overlays | Give vanilla references `(campaign, original form)` identities; isolate NPC death/enable state, inventories, globals, factions, locations, doors, and encounters. Current instance filtering alone hides shared references. |
| Dialogue and story | Implement topic/response selection, conditions, aliases, scenes, packages, story-manager events, radiants, followers, and quest item/reward behavior. Every mutating effect must carry campaign context. |
| Client engine | Port/audit Skyrim Platform for installed runtime 1.7.104; reset/rebind local world state when changing phases; integrate journal/objective markers without rerunning client fragments; acknowledge transitions. See [the native runtime audit](CLIENT_RUNTIME.md). Current native build is not proven compatible with that runtime. |
| In-game quest coverage | For every quest and relevant branch, exercise solo and guest play, divergent histories, late joins, party removal, disconnects and restart mid-scene/mid-reward on actual Skyrim clients. Individual quests are integration tests; the completion target is the full five-master campaign. |
| Full content | Cover main quest, guilds, civil war, side/radiant quests, and DLC. Track branches and inter-quest dependencies rather than assuming successful SetStage equals support. |
| Community release | Test 16/32 real or representative clients, load/recovery, reward idempotency, content-version agreement, authentication, and packaging. |

The next execution slice needs campaign reference overlays and alias resolution, then lifecycle execution around them. Vanilla stage fragments already expose these dependencies. A global reference plus one progress map cannot resolve conflicting NPC states.

## Verification commands

```sh
# Node 24: includes TypeScript replica tests
node --test misc/boyhouse/partyInstances.test.cjs misc/boyhouse/instanceCommand.test.cjs misc/boyhouse/questJournalFeed.test.cjs misc/boyhouse/questJournalReplica.test.cjs

# Linux builder with owned masters mounted at /game-data
./build/unit/unit '[Quest]'
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs
```

The native suite also parses every owned quest's VMAD and every PEX in the owned vanilla script archive, executes original MQ102/DA04 fragments, and checks shared-heap persistence, old-save migration, reset collection, conditional guest execution, nested same/cross-quest stage calls, failure rollback and instruction budgets. The addon smoke exercises original-fragment execution through the public server binding. Protocol bots exercise the full server login/party flow and rejected client writes. None of these substitutes for an in-game quest playthrough.
