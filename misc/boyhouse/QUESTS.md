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

`mp.set(actorId, "questCommand", { quest: 0x4e50d, op: "setStage", index: 20, expectedRevision: 7 })` is a trusted gamemode operation, never a client command. Supported operations are `start`, `stop`, `reset`, `complete`, `setActive`, `setStage`, `setObjectiveDisplayed`, `setObjectiveCompleted`, `setObjectiveFailed`, `completeAllObjectives`, and `failAllObjectives`. Boolean operations take `value`; objective/stage operations take `index`. `expectedRevision` is optional optimistic concurrency protection. Repeated visited stages do not rerun or advance the revision. Repeated-stage execution semantics still need the fragment runner.

Mutations require the owner's private `private.boyhouse.questAdapters` array to contain the quest's stable key. This is a trusted adapter opt-in, **not automatic evidence that an adapter is correct**. No production vanilla adapter is supplied or enabled. The smoke/unit fixtures explicitly opt in MQ102 only to validate record indices and journal plumbing; they do not play or certify Before the Storm.

The persisted field is `private.boyhouse.questJournal`. Unknown save versions are refused rather than reset. The implementation validates before writing and uses the existing character save path; it does not yet provide atomic transactions spanning quest state, inventory rewards, and world overlays. The ledger limit is 8 MiB. The diagnostic feed currently sends full snapshots, capped at 1 MiB; exceeding that limit returns unavailable rather than truncating progress. Delta/chunk transport is required before full campaign rollout. VM epochs are process-local guards, not network transition acknowledgements or VM persistence.

## Remaining implementation gates

| Gate | Required work and evidence |
| --- | --- |
| Original quest execution | Parse QUST VMAD fragment/alias metadata; create campaign-scoped quest script objects; bind original owned PEX scripts and properties; execute conditional stage fragments, start/stop events, and repeatable stages. Today `EspmGameObject` cannot host quest scripts. |
| VM persistence | Persist variables, aliases, timers, and latent continuations with explicit campaign identity. Restart during a stage/reward must neither lose nor duplicate effects. |
| World overlays | Give vanilla references `(campaign, original form)` identities; isolate NPC death/enable state, inventories, globals, factions, locations, doors, and encounters. Current instance filtering alone hides shared references. |
| Dialogue and story | Implement topic/response selection, conditions, aliases, scenes, packages, story-manager events, radiants, followers, and quest item/reward behavior. Every mutating effect must carry campaign context. |
| Client engine | Port/audit Skyrim Platform for installed runtime 1.7.104; reset/rebind local world state when changing phases; integrate journal/objective markers without rerunning client fragments; acknowledge transitions. Current native build is not proven compatible with that runtime. |
| First playable quest | Run one real quest solo and with guests; test divergent stories, late joins, party removal, disconnects, and restart mid-scene/mid-reward on actual Skyrim clients. Record branch evidence before marking it supported. |
| Full content | Expand to main quest, guilds, civil war, side/radiant quests, and DLC. Track branches and inter-quest dependencies rather than assuming successful SetStage equals support. |
| Community release | Test 16/32 real or representative clients, load/recovery, reward idempotency, content-version agreement, authentication, and packaging. |

The next execution slice should implement a scoped quest object and fragment runner for a small deterministic fixture before enabling any vanilla quest. A global quest object plus one progress map cannot isolate original script variables or resolve conflicting NPC states.

## Verification commands

```sh
# Node 24: includes TypeScript replica tests
node --test misc/boyhouse/partyInstances.test.cjs misc/boyhouse/instanceCommand.test.cjs misc/boyhouse/questJournalFeed.test.cjs misc/boyhouse/questJournalReplica.test.cjs

# Linux builder with owned masters mounted at /game-data
./build/unit/unit '[Quest]'
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs
```

The native suite covers owner/guest/stranger separation, journal serialization, invalid writes, stale contexts, Papyrus VM native dispatch, and condition predicates. The addon smoke connects real native storage and party decisions to the journal feed. Protocol bots exercise the full server login/party flow and rejected client writes. None of these substitutes for an in-game quest playthrough.
