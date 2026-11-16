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

- Bounds-checked QUST VMAD versions 4/5, both object layouts, properties/arrays, stage/log-entry bindings, and alias script metadata. Parsed values own their storage. Alias VM objects and attached scripts now have campaign identities; world filling and lifecycle remain partial (see below).
- Fresh, campaign-scoped quest objects and a strict VM for each transaction. Identical quest form IDs in different campaigns do not share Papyrus identities or variable maps.
- Original PEX loading, including BSA decompression. The reader checks headers, bounds, string-table indices and call argument counts, handles absent debug information, and distinguishes parameters from locals.
- Conditional log-entry selection using the four quest predicates, campaign globals, and conditional quest script variables, in the host's context for guests. Unsupported condition functions, run-on contexts, and log flags are rejected. Matching fragments execute in log-entry order; the complete-quest log flag updates the same transaction.
- Primitive variables, shared arrays, script state and quest-object references persisted with the journal. Variables are qualified by attached script and declaring class; references use stable plugin/form keys. Private VM state is omitted from client snapshots. Resetting a quest clears its saved script variables and collects arrays only when no other script references them.
- Campaign-owned GLOB objects and `GlobalVariable.GetValue`, `GetValueInt`, `SetValue`, `SetValueInt`, and `Mod`. Defaults come from owned FLTV records; writes use float32 storage, stable plugin/form keys and the same transaction as stages/variables. A quest reset preserves global effects. The scoped runner's `GetGlobalValue` condition (index 74) sees earlier uncommitted writes, including during nested stages. Global values remain private server state; native client/NPC world projection is not implemented.
- `GetVMQuestVariable` (index 629) reads the owned CIS2 variable name from the active campaign's conditional quest script. Int/Float/Bool declarations, case-insensitive names, inherited variables, PEX defaults and persisted values are supported. Conditional flags come from each PEX's own user-flag table. The target must be running and adapter-enabled; missing/ambiguous scripts, nonconditional variables, type mismatches and nonfinite values fail the transaction. This does not initialize a quest or run property getters.
- Stage conditions may compare against a GLOB (`UseGlobal`). The comparison is resolved in the same campaign immediately before each log, so earlier fragments' uncommitted writes affect later branches. Other special condition flags remain unsupported. Regressions use MQ101's original faction-path conditions and CWObj's original cross-quest/global comparisons with controlled fragment bodies, including character reload, guest isolation and rollback. They are condition tests, not playthroughs of those quests.
- Nested `SetStage`/`SetCurrentStageID` calls execute target fragments in the same VM, campaign, instruction budget and transaction. Cross-quest calls require the target quest's own adapter opt-in. Visited stages break cycles; invalid targets and failures roll back every affected quest. `Start`, `Stop` and `Reset` execution still require lifecycle/event support.
- One in-memory commit for journal changes, script variables and the supported inventory natives described below. Unsupported natives, missing bindings, stale contexts, invalid property targets, invalid array access, runaway execution, and latent calls throw and discard the transaction. A VM-wide instruction budget also covers nested calls and synchronous inventory handlers. Other world-effect natives remain absent until their effects can participate in the transaction.

Original MQ102 stage 100 and DA04 stage 5 are execution fixtures. The DA04 fragment displays objective 5; a guest's execution changes only the host's journal. MQ102 stage 100 has no visible objective effect from a fresh journal. Other stages reach cross-quest adapter gates, map-marker effects, alias operations and nested lifecycle execution. Their first failure can move as earlier dependencies are implemented. Those operations fail rather than being omitted. These fixtures do not start or play the quests: no initialization events, complete alias filling/effects, dialogue, scene execution, rewards, or world overlays are supplied. Repeated stages are currently idempotent, even for quests that eventually need repeatable-stage semantics.

VM save schema 4 separates script roots, a campaign-local array heap, campaign globals, and resolved alias bindings. Schema 2/3 saves and legacy inline saves migrate on a successful transaction. Multiple variables/scripts retain one allocation across fragments and character reloads, including mutations through a script whose other sharers are unloaded. Equal-valued allocations remain separate; null and empty arrays remain distinct. Unreachable allocations are collected. Papyrus equality compares array identities and recognizes typed null arrays as None. The runner remains an experimental subset; latent stacks, timers and object lifecycle persistence are unfinished.

### Campaign forms and alias subset

The isolated runner represents static forms as `(campaign owner, original FormID)` and aliases as `(campaign owner, quest, alias ID)`. These are logical Papyrus objects, **not live NPC/reference clones**. `Game.GetPlayer` binds canonical PlayerRef to the campaign owner even when a guest triggers execution. `Game.GetForm`, `Form.GetFormID`, `ObjectReference.GetBaseObject` and `Actor.GetActorBase` read owned static identities; dynamic form lookup is rejected until campaign allocation exists.

Placed reference objects now bind their original base/reference scripts to the
campaign VM. Reference properties override base properties, with every object or
array element mapped through the plugin that supplied it. Both removed-script
flags and removed properties are honored. NPC `UseScript` inheritance follows
deterministic NPC templates; leveled template selection, cycles and invalid
bindings fail. Missing attached PEX is an error, including when encountered
while resolving a reference property.

Reference script variables and arrays persist under `reference:<FormDesc>/<script>`
roots in the same transaction as the journal. Different quests in one campaign
share that reference's state; resetting a quest preserves it. Host/guest histories
stay separate, character reload retains it, and fragment failure rolls it back.
Resolving these logical bindings does not instantiate NPCs or dispatch events.
Explicit private copies have the limited load dispatcher described below;
ordinary global-VM world mutations remain prohibited. PlayerRef base bindings and
active-magic-effect instances remain unimplemented. Content migration and
reference retirement still need explicit lifecycle handling.

The strict campaign VM scopes method calls to their declared receiver and
executing attachment. Inherited calls retain virtual `self` dispatch; explicit
`Parent` calls stay within that attachment's parent chain. Native declarations
must reach a registered implementation, never execute an empty PEX body. State
is shared with inherited methods in the same attachment. Shared engine helpers
such as `Quest.SetStage` use their registered campaign implementation when several
attachments inherit the same declaration; distinct script implementations remain
ambiguous. State and method names are case insensitive, and explicit state
callbacks use their actual receiver. Resolved `self` property operands retain
their attachment too.
Inheritance cycles, missing parents, invalid calls and ambiguous bindings fail
the transaction. Casts can reach a unique attached custom-script ancestor.
Dynamic script-handle identity across casts/assignments is not persisted yet;
ambiguous sibling inheritance remains unsupported rather than choosing by
attachment order. These changes do not add lifecycle/event scheduling or durable
latent execution.

`Quest.GetAlias`, alias `GetOwningQuest`/`GetID`/`GetName`, reference alias getters and `LocationAlias.GetLocation` are available. VMAD alias properties and `GetAlias` share one object identity. Attached alias scripts load from owned PEX and persist separately from quest scripts. Resolving an object does not dispatch initialization, fill or other lifecycle events.

For an adapter-enabled running quest, target getters can resolve forced references/locations, unique actors with exactly one winning nondeleted reference, and external aliases. Resolved targets persist as stable plugin/form keys; explicit empty bindings remain distinct from unresolved aliases. Stopped/unstarted quests return None. Unknown fills, conditions, creation, propagation, ambiguous actor references and cycles fail the transaction. Reflection alone does not fill aliases. Alias flags, reservations, factions, inventory, spells, packages and other lifecycle effects are retained as metadata but **are not applied**. These APIs remain inside the existing experimental adapter boundary, with no vanilla adapters enabled.

The bounded metadata parser reads every owned quest alias, including signed creation descriptors, reference/location distinctions, optional legacy inventory counts, conditions and unknown fields. The owned catalog contains 14,514 reference aliases and 1,237 location aliases. This is format coverage, not playable quest coverage.

Alias bindings, alias-script variables, globals and journal changes commit together with supported inventory effects. Failure after a binding, script or inventory mutation rolls back the whole transaction. Character reload retains bindings; quest reset removes that quest's bindings and script roots while preserving other quests and campaign globals. General live world-state transactions remain future work.

### Inventory effects and explicit private references

The isolated runner implements partial `ObjectReference.GetItemCount`, `AddItem`
and `RemoveItem` for concrete base items. Reads see staged changes; transfers
preserve the inventory model's extra data. PlayerRef means the campaign owner,
including when a guest triggers the fragment. Other references must have an
explicit, validated `CampaignReferenceBinding` to a dynamic copy in the owner's
current private phase. Shared originals and other players cannot be targets.
Bindings persist with a random character identity to prevent a reused character
FormID from resolving old copies. This does not allocate copies automatically or
solve orphan visibility and instance naming after character-ID reuse.
Binding refuses a copy with existing shared-VM script instances. Bound copies
reject ordinary script initialization and shared-VM event entry; unsupported
external events cannot silently run their scripts in the global VM.

### Private reference loading and recovery

The first player subscription to an explicitly bound copy runs a scoped load
transaction before publishing that subscription. `OnInit` runs once per original
reference in the owner's saved campaign; `OnCellLoad` and `OnLoad` follow it.
Reference scripts receive all three events. Matching scripted reference aliases
of running, adapter-enabled quests receive the load events, but not the target's
`OnInit` (alias initialization belongs to quest startup). Inventory events caused
by these handlers drain within the same transaction and execution budget.

Initialization markers, script state and supported inventory effects commit
together. A failed handler leaves the load retryable without retaining its
reward or initialization marker. The private `referenceInit` ledger field
survives restart and is omitted from the client journal projection. This hook
covers the first subscription in each server process, not a complete engine
cell attach/detach/unload lifecycle or a durable external event queue.

Saved private copies restore directly, without rerolling their inventory,
resetting attributes, opening character creation or scheduling dead NPCs to
respawn. Copies may load before their owner; their scripts wait for a player
subscription. Bound placed-item copies are retained by database loading too.
Recovery validates the original record, base, copy kind, owner identity and
private phase. Pending loot timers and active magic effects fail explicitly
until campaign timer recovery is implemented. Automatic area-entry integration,
complete quest startup, trigger/activation/combat events and ordinary player inventory
events still need implementation; no vanilla adapter is enabled.

### Creating private reference copies

Trusted server code can now call `mp.set(owner, "campaignReferences", {
references: [originalFormId, ...], encounterLevel: 5 })` while the owner is in
their own private phase. Alternatively supply `area: { worldOrCell, center:
[x, y, z], radius }` instead of `references`; the plugin spatial index discovers
winning placed records inside that sphere without loading shared references.
The caller must supply the authoritative encounter level. This is not a client
command.

Supplying `instanceArea: "area_id"` prepares that owner's destination without
changing their current phase. This is also valid while they are in shared space
or visiting another campaign: allocation reads their personal saved globals,
never the host's. The destination key is derived from the owner ID; callers
cannot supply an arbitrary other owner's phase. Existing copies in a different
area are still refused until cross-area migration exists.

Party entry, revocation and pre-spawn recovery now prepare the configured area
before changing native visibility. Each configured area requires a trusted
integer `encounterLevel` from 1 through 65,535; existing deployments must add this
field before starting this version. Preparation failure during voluntary entry
leaves the prior visit intact. Failure during revocation/recovery, or after a
native transition begins, disables the character, removes its visit/party
membership, detaches native actor authority and closes its connection. A host
leaving an area or visiting another campaign returns remaining guests to their
own prepared versions. A 250 ms reconciliation loop handles boundary departures
and missing hosts; recovery outside a saved boundary returns to shared space.
First entry still uses the explicit instance command. Automatic entry and
client acknowledgement require the projection protocol below.

The factory creates unique/story NPCs, disabled references and placed item stacks,
which the ordinary shared loader may skip. It selects template chains, inventory,
outfits, spells and starting corpse loot; reads initial enable-parent state; and
copies factions and the currently implemented actor attributes. The owner gets a
private allocation ledger mapping stable original forms to dynamic copies.
Subsequent calls reuse those copies, including their changed inventories and
deletion state. Missing allocations fail instead of silently regenerating loot.
Copies can recover before their owner and keep their original allocation results.

Preparation builds replacement form, spatial and identity indexes and the complete
pending save batch before publishing anything. The owner's identity/ledger and
new copies enter one atomic storage batch. Creation runs no load handlers or
shared VM scripts; the existing subscription lifecycle invokes campaign handlers
later. Calls from inside a quest transaction are refused until allocation can
participate in that transaction. Notifications still precede asynchronous disk
commit; see [the storage contract](STORAGE.md).

This does not yet project original-to-copy replacement into Skyrim or make a
location playable. Automatic entry, engine-side transition application, cross-area migration,
enable-parent change propagation, scale/ownership/lock overlays, NPC auto-calculated
stats, respawns and durable lifecycle delivery remain unfinished. The factory
currently copies the relevant world indexes during a batch; this needs measurement
and optimization before a 16–32 player rollout. Requests are bounded to 4,096
references, a radius of 65,536 units, and a 100,000-entry/8 MiB owner ledger;
streaming and ledger partitioning are required for a full world.

### Reference projection handshake (experimental)

`mp.get(actorId, "campaignReferences")` returns a read-only, versioned manifest
of the current campaign's original-to-copy bindings, base IDs, reference kinds,
and disabled/deleted state. Shared space returns an empty manifest. It validates
saved allocations and campaign identity without creating or repairing them, and
never includes private VM or inventory data.

With `boyhouseInstances.projection: true`, each connection starts with native
incoming gameplay blocked. The server sends a bounded manifest in 128-reference
chunks and accepts an acknowledgement only for that connection's session,
sequence and unpredictable token. It rechecks the current native manifest before
releasing gameplay. Phase changes close the gate before changing the instance;
timeouts and reported application failures quarantine the character. A host
awaiting application cannot admit new guests. Existing guests remain admitted
while their host refreshes the same phase.

The pure client replica validates and assembles chunks, but acknowledges only
when an engine adapter explicitly reports application complete. The protocol
smoke uses real native bot packets and simulates that engine completion. It is
not an in-game test. Incoming gameplay gating does not freeze server simulation
or outgoing replication. The Skyrim engine adapter, transition coordinator and
reference restoration are still unfinished. Keep the flag unset in staging and
normal clients until those are implemented and tested. No quests are certified.

### Packet epochs (transport implemented; transition integration pending)

Trusted `mp.beginPacketEpoch(userId)` requires a connected user with gameplay
blocked. It creates a fresh, random 128-bit token; `mp.getPacketEpoch(userId)`
reads it. A new connection starts without a token and retains legacy transport.
No production coordinator calls these methods yet, including the experimental
projection feed. The manifest acknowledgement token and packet epoch currently
remain separate facilities.

Once selected, the native server requires the current token on all gameplay
input, including custom commands, even after the gameplay gate reopens. Outbound
replication carries the recipient's token. `CampaignFrame` (message type 34)
retains the original binary payload and reliability, adding 22 bytes. Frames are
bounded to a 1 MiB payload; nested, truncated and trailing frames are rejected.
Projection control stays unframed; initial login may pass before actor binding.
Deferred messages capture the enqueue-time token and are dropped after rotation
instead of being relabeled as current state. A fresh baseline must replace them.

The client's `NetworkingService` routes structured, raw and reference-ID sends
through a common gate. Explicit selection queues only that epoch's replication,
bounded to 4,096 messages and 8 MiB of estimated text. Gameplay input is dropped
while paused, and resumes only after explicit release drains the queue. A new
selection/disconnect cancels old replay. Old and untagged gameplay packets remain
rejected after release. Failures close input and schedule connection cleanup;
cleanup from an obsolete connection cannot close its replacement. The native
client retains the delivering connection until `Tick` returns and stops its old
packet batch when a callback replaces or destroys it.

Epoch transport requires the matching `MpClientPlugin` 2.1.0 codec; older codecs
are refused with input closed. Neither receipt of a frame nor a manifest enables
gameplay. The coordinator still must rotate before allocation/phase changes,
send a fresh authoritative baseline after epoch selection, apply/restore engine
references, and cancel stale asynchronous engine work before acknowledging and
releasing. Large replication baselines need bounded batching and explicit failure
handling. These tests exercise the transport and service wiring, not Skyrim's
engine or a playable quest.

### Deferred client effect lifetime

Queued native client snippets now capture the actor's registered lifetime and
phase epoch. Campaign guests also capture the host's lifetime and epoch. Leaving
and returning to the same phase does not revive an earlier command. Deferred
inventory snapshots remain personal to the same actor, but cannot be delivered
to a replacement actor that reused its numeric ID.

Pending snippet replies undergo the same context checks. Expired waits reject
on the next connected-user tick even if no reply arrives; disabling an actor
rejects waits instead of resuming the script with a fabricated None result.
Continuations are removed before callbacks so callbacks can create more work or
disable the actor without invalidating the pending map. Signed or oversized wire
indices cannot alias another wait after narrowing to 32 bits.

This guards server queues and continuations. It cannot retract packets already
sent or cancel Skyrim-side callbacks. The packet transport above rejects stale
epochs once enabled, but connection ownership changes and engine-side
cancellation still require the transition integration.
The client transport currently uses RELIABLE (without ordering) for reliable
outbound messages, while the server uses RELIABLE_ORDERED. Unreliable movement
can arrive across a phase boundary; ordering must not be assumed as a substitute
for context validation.

### Inventory defaults for private reference allocation

The shared spawn helpers now preserve nested leveled-entry quantities, apply
the nearest eligible level tier unless all lower tiers are requested, and read
chance-none globals instead of treating a global as an unconditional empty roll.
Callers can supply campaign globals and an allocation RNG. Recursive expansion
rejects cycles, missing records, count overflow and excessive work; NPC template
chains reject cycles and empty or multiple-actor selections instead of hanging.

Leveled records are decoded field by field, including lists with interleaved
ownership metadata and empty lists without a count field. Container and inherited
NPC inventory IDs use the supplying plugin's load-order mapping. An outfit is
rolled once and the result is reused for inventory and equipment.

The factory persists selected templates and inventory in each copy; authoritative
campaign encounter levels and respawns still need their own lifecycle.
Special-loot formulas and ownership metadata semantics are
not implemented; UseAll level behavior still needs comparison with the engine.
An empty leveled NPC template currently fails explicitly rather than silently
choosing a different actor. The scoped Papyrus inventory API still rejects
leveled-item arguments. No additional quest is enabled by these changes.

### Transaction-generated inventory events

Transaction-generated `OnItemAdded`/`OnItemRemoved` events run on the logical
reference's script attachments and matching scripted aliases of running,
adapter-enabled quests. Active-state and inherited handlers execute independently
for each attachment. Events share the stage's transaction and execution budget;
unsupported alias fills, missing natives, latent handlers and unbounded event
generation roll back inventory, journal and script state together. Ordinary
player/world inventory events do not yet enter this dispatcher.

FormLists, leveled items, item-reference arguments, transactional unequip,
inventory-event filters, pickup UI messages and the full quest-item/ownership
model are unsupported. Commit requires atomic persistence when storage is
attached; notifications currently precede asynchronous disk commit. See the
[storage contract](STORAGE.md). No adapter is enabled by this inventory subset.

Scene and TopicInfo `GetOwningQuest` now return a quest object in the active campaign. Topic ownership follows its winning DIAL record; scene package-action PNAM fields are distinguished from the parent QUST. The regression invokes objective changes through those returned objects during guest play and checks personal-history isolation and rollback. Dialogue selection, scene actions and their event dispatch are not implemented by these getters.

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

The persisted field is `private.boyhouse.questJournal`. Unknown save versions are refused rather than reset. The implementation validates before writing and uses the existing character save path. The optional [SQLite driver](STORAGE.md) commits each complete save batch atomically across change forms. The supported inventory subset stages rewards with journal/VM state, but general world overlays and acknowledgements after disk commit remain unfinished. The ledger limit is 8 MiB. The diagnostic feed currently sends full snapshots, capped at 1 MiB; exceeding that limit returns unavailable rather than truncating progress. Delta/chunk transport is required before full campaign rollout. VM epochs are process-local guards, not network transition acknowledgements or VM persistence.

## Catalog-wide automation

The [dependency analyzer](DEPENDENCIES.md) discovers shared work across the full catalog; the [ranked backlog](QUEST_BACKLOG.md) records potential impact. It resolves load-order overrides, associates dialogue/scenes/story nodes, reads VMAD/PEX metadata, and traces typed calls to engine APIs. Unknown dependencies and runtime-dispatch candidates remain explicit. This automates discovery and regression inputs; it does not replace engine implementation or playthrough evidence.

## Remaining implementation gates

| Gate | Required work and evidence |
| --- | --- |
| Original quest execution | Extend the implemented scoped fragment runner with startup/stop events, alias filling and alias scripts, nested quest lifecycle transitions, repeatable stages, and the remaining condition/native semantics. The existing global VM must not become an alternate campaign-effect path. |
| VM persistence | Shared arrays, variables, globals and alias bindings now persist; add alias lifecycle, timers, latent continuations, content hashes and content migrations. Restart during a stage/reward must neither lose nor duplicate effects. |
| World overlays | Explicit entry/recovery prepares private references; add automatic area entry and client projection, then extend isolated death/enable state, inventories, globals, factions, locations, doors, and encounters with their full mutation/event semantics. Filtering alone hides shared references. |
| Dialogue and story | Implement topic/response selection, conditions, aliases, scenes, packages, story-manager events, radiants, followers, and quest item/reward behavior. Every mutating effect must carry campaign context. |
| Client engine | Port/audit Skyrim Platform for installed runtime 1.7.104; reset/rebind local world state when changing phases; integrate journal/objective markers without rerunning client fragments; acknowledge transitions. See [the native runtime audit](CLIENT_RUNTIME.md). Current native build is not proven compatible with that runtime. |
| In-game quest coverage | For every quest and relevant branch, exercise solo and guest play, divergent histories, late joins, party removal, disconnects and restart mid-scene/mid-reward on actual Skyrim clients. Individual quests are integration tests; the completion target is the full five-master campaign. |
| Full content | Cover main quest, guilds, civil war, side/radiant quests, and DLC. Track branches and inter-quest dependencies rather than assuming successful SetStage equals support. |
| Community release | Test 16/32 real or representative clients, load/recovery, reward idempotency, content-version agreement, authentication, and packaging. |

The next execution slice needs area-entry/client integration for materialized references, complete alias fill/effect semantics, and lifecycle execution around them. Vanilla stage fragments already expose these dependencies. A global reference plus one progress map cannot resolve conflicting NPC states.

Transition integration must establish a new packet generation before allocation or phase changes emit reference updates, carry that generation on gameplay packets in both directions (including binary movement), and provide a fresh authoritative replication baseline when a manifest is superseded. The client must bound its pending queue, discard old generations, and guard delayed engine callbacks. Only after original-reference replacement/restoration finishes may it acknowledge and resume input. A manifest receipt alone is insufficient.

## Verification commands

```sh
# Node 24: includes TypeScript replica tests
node --test misc/boyhouse/partyInstances.test.cjs misc/boyhouse/instanceCommand.test.cjs misc/boyhouse/questJournalFeed.test.cjs misc/boyhouse/questJournalReplica.test.cjs misc/boyhouse/worldProjection.test.cjs misc/boyhouse/campaignPacketChannel.test.cjs

# Linux builder with owned masters mounted at /game-data
./build/unit/unit '[Quest]'
./build/unit/unit '[CampaignFrame]'
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs --projection
```

The native suite also parses every owned quest's VMAD and every PEX in the owned vanilla script archive, executes original MQ102/DA04 fragments, and checks shared-heap persistence, old-save migration, reset collection, conditional guest execution, nested same/cross-quest stage calls, failure rollback and instruction budgets. Alias regressions cover owned metadata, host/guest identities, target and script-variable persistence, reset, stopped/cross-quest boundaries, invalid saves and dynamic lookup rejection. The addon smoke exercises original-fragment execution through the public server binding. Protocol bots exercise the full server login/party flow and rejected client writes. None of these substitutes for an in-game quest playthrough.
