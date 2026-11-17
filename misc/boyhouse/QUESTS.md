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
- Conditional log-entry selection using the four quest predicates, campaign globals, conditional quest script variables, and the five bounded physical/faction predicates below, in the host's context for guests. Unsupported condition functions, run-on contexts, and log flags are rejected. Matching fragments execute in log-entry order; the complete-quest log flag updates the same transaction.
- Primitive variables, shared arrays, script state and quest-object references persisted with the journal. Variables are qualified by attached script and declaring class; references use stable plugin/form keys. Private VM state is omitted from client snapshots. Resetting a quest clears its saved script variables and collects arrays only when no other script references them.
- Campaign-owned GLOB objects and `GlobalVariable.GetValue`, `GetValueInt`, `SetValue`, `SetValueInt`, and `Mod`. Defaults come from owned FLTV records; writes use float32 storage, stable plugin/form keys and the same transaction as stages/variables. A quest reset preserves global effects. The scoped runner's `GetGlobalValue` condition (index 74) sees earlier uncommitted writes, including during nested stages. Global values remain private server state; native client/NPC world projection is not implemented.
- `GetVMQuestVariable` (index 629) reads the owned CIS2 variable name from the active campaign's conditional quest script. Int/Float/Bool declarations, case-insensitive names, inherited variables, PEX defaults and persisted values are supported. Conditional flags come from each PEX's own user-flag table. The target must be running and adapter-enabled; missing/ambiguous scripts, nonconditional variables, type mismatches and nonfinite values fail the transaction. This does not initialize a quest or run property getters.
- Stage conditions may compare against a GLOB (`UseGlobal`). The comparison is resolved in the same campaign immediately before each log, so earlier fragments' uncommitted writes affect later branches. Other special condition flags remain unsupported. Regressions use MQ101's original faction-path conditions and CWObj's original cross-quest/global comparisons with controlled fragment bodies, including character reload, guest isolation and rollback. They are condition tests, not playthroughs of those quests.
- `GetDisabled` (35) and `GetItemCount` (47) accept explicit Run On Reference targets; `GetDead` (46) also accepts Run On Quest Alias when the owning quest's existing alias resolver supplies a logical actor reference. Alias ID zero is valid and comes from CTDA byte 28, not the reference field at byte 24. PlayerRef resolves to the campaign owner; other evaluated targets require a bound private copy in that owner's current area. Evaluated missing copies, empty or unsupported alias fills, and nonactor `GetDead` targets fail the transaction. Implicit Subject/Target, dialogue, scenes, package conditions, and alias-fill condition execution remain unsupported.
- Physical conditions use tracked reference snapshots and the transaction's staged inventory/visibility values. Item counts require initialized private inventories and the same concrete-item subset as inventory natives (including carryable lights, excluding FormLists, leveled lists and currently keys). Counts above signed-int32 range fail; comparisons retain exact integer counts. The compiler validates every selected-stage clause, including clauses skipped by OR. Comparison globals, item metadata and explicit target record types are checked before each log's short-circuit evaluation: non-player targets must be live placed REFR/ACHR records, and `GetDead` requires ACHR. Skipped branches do not resolve private copies or fill aliases. These predicates do not add death simulation, visibility propagation or alias lifecycle effects.
- Nested `SetStage`/`SetCurrentStageID` calls execute target fragments in the same VM, campaign, instruction budget and transaction. Cross-quest calls require the target quest's own adapter opt-in. Ordinary visited stages do not rerun; authored repeatable stages can execute again as described below. Invalid targets and failures roll back every affected quest. `Start`, `Stop` and `Reset` execution still require lifecycle/event support.
- One in-memory commit for journal changes, script variables, supported inventory effects and the private-copy visibility subset described below. Unsupported natives, missing bindings, stale contexts, invalid property targets, invalid array access, runaway execution, and latent calls throw before publication and discard the transaction. A VM-wide instruction budget also covers nested calls and synchronous inventory handlers. Postcommit replication failures quarantine observers; they do not undo an already published reward or visibility change.

Original MQ102 stage 100 and DA04 stage 5 are execution fixtures. The DA04 fragment displays objective 5; a guest's execution changes only the host's journal. MQ102 stage 100 has no visible objective effect from a fresh journal. Other stages reach cross-quest adapter gates, map-marker effects, alias operations and nested lifecycle execution. Their first failure can move as earlier dependencies are implemented. Those operations fail rather than being omitted. These fixtures do not start or play the quests: no initialization events, complete alias filling/effects, dialogue, scene execution, rewards, or world overlays are supplied.

VM save schema 6 separates script roots, a campaign-local array heap, campaign globals, resolved alias bindings, and explicitly captured script stacks. Schema 2/3/4/5 saves and legacy inline saves migrate on a successful transaction. Multiple variables/scripts retain one allocation across fragments and character reloads, including mutations through a script whose other sharers are unloaded. Equal-valued allocations remain separate; null and empty arrays remain distinct. Unreachable allocations are collected. Papyrus equality compares array identities and recognizes typed null arrays as None. The runner remains an experimental subset; persistent quest jobs, timers and object lifecycle are unfinished.

### Saved script stacks

The interpreter exposes explicit `StartResumableFunction` and `ResumeFunction`
host entry points for an attached script function. An admitted native can request
a terminal suspension with an operation token and result type. Each frame records
its stable receiver identity, script attachment and implementation, function kind,
function/state, call instruction, and typed locals/parameters. A single campaign
scope, operation token and instruction budget cover the leaf and all its callers.
Resumption supplies the native result and continues after the saved call; it
does not rerun the instructions before that call.

Every script in the attachment's inheritance chain has a streamed SHA-256 of
its current executable metadata. Restore preflights the entire saved stack:
hashes, receivers and campaign scope, local layouts/types, call destinations,
caller links and remaining budget, before executing the leaf.
The hash includes code, declarations, defaults, properties and flags; it omits
nonexecuting build timestamps, debug information and documentation. It is an
execution identity check, not a content migration or a complete mod manifest.

Schema 6 stores stacks with a versioned, bounded codec: at most 128 frames and
65,535 locals/parameters across the stack. Schema 5 leaf nodes migrate only if
their old shape is valid; unknown fields or versions are rejected. Local-only
arrays in any caller remain heap roots; locals and script fields sharing an
array retain one allocation after reload. Taking a stack checks its scope and consumes its pending token;
a second take fails. Put/take validate a candidate before changing the heap.
Reset refuses outstanding frames until cancellation and dependency semantics
exist, including frames whose scope alone does not reveal every referenced quest.

This entry path supports nested methods, static helpers, inherited Parent calls,
and property getters/setters. Resumption returns through callers in order; a
second suspension retains the callers that have not run yet. A setter's return
does not overwrite its input operand. Saved self/local receiver links are
validated; a mutable field is not re-dispatched because a callee may have changed
that field before suspension. Native C++ caller work is not serializable:
nested native-to-script-to-native suspension remains rejected.
The admitted native itself must use the request as terminal control flow; it must
not suppress the request or expect C++ code after the await to run on resume.
Ordinary strict-mode promises remain rejected. The host is responsible for
rebinding stack policy and committing token consumption, resumed effects and any
replacement frame in one campaign transaction.

The existing `ExecuteStage` API remains synchronous. The experimental durable
coordinator below uses these entry points, but no production Utility.Wait,
Start/Stop/Reset or latent event is enabled. Lifecycle ordering and real native
operation delivery remain unfinished. See [STORAGE.md](STORAGE.md).

### Durable stage work

`world.GetQuestJobs()` exposes a trusted C++ `QuestJobRunner`. `Submit` accepts a
campaign actor, unique command string, quest and stage; `Inspect` reports that
command's status. `Resume` accepts its pending token and completion. These APIs
are not client packets or addon bindings. The explicit metadata/script overloads
are fixtures: only they register `FixtureAwait.AwaitBool`, with a bounded string
key and Boolean result. The ordinary overload pins owned content and registers
no replacement for a missing Skyrim native.

Every segment creates a fresh isolated VM inside `AtomicJob`, saves script
fields, shared arrays, complete caller stacks, the selected log/fragment and
pending inventory events, then seals an exact durable save receipt. A resumed
log does not repeat its condition, QSDT completion or earlier fragment effects.
The next operation token and cumulative instruction/work budgets survive reload.
Inventory handlers remain synchronous and drain after the stage finishes; a
latent handler or nested native-to-script wait still fails explicitly.

Private journal schema 2 adds validated job state; ordinary untouched journals
remain schema 1. Public snapshots explicitly project the existing schema-1
progress/context fields, excluding jobs, operation tokens and the VM. Job and
stack ownership, outer fragment binding, content signatures and budgets are
validated together at commit. Failed resumed execution keeps the last saved
checkpoint and effects intact. There is no automatic retry of script execution.

`Pending` exposes no operation token or final result. `Waiting` and `Completed`
are visible only after the matching receipt becomes durable. After a fresh
server loads persisted jobs, their saved state replaces process-local receipts.
Duplicate commands/completions report existing work; conflicting values fail.
Operation tokens are campaign-scoped; a future external dispatcher must include
the owning campaign in its delivery/deduplication identity.
Clearing a world stops its coordinator: recovery requires a fresh server after
the previous writer has shut down, rather than reusing abandoned write state.

The signature covers ordered complete-master SHA256 values, plugin mapping,
raw quest fields, supplied VMAD metadata and executable script hashes. Production
scripts are pinned once per coordinator; master hashes are cached per immutable
loader. Master files must not change while loaded. Changed content is refused,
not migrated. The initial full script parse/hash is not yet a performance-tested
startup path.

One job owns a campaign at a time. Ordinary quest mutation and private-reference
allocation cannot overwrite active work, including a character's personal
history while visiting another host. This does not provide a global gameplay
lease over every physical action between segments. Completed history currently
has a hard 256-command limit with no eviction; new work is refused at capacity
to avoid replaying old rewards. Scalable deduplication retention, real external
operation delivery, child-stage scheduling, latent event cursors, lifecycle
semantics and real-client playthroughs remain required for full quest support.

### Repeatable stages and lifecycle metadata

The runner honors the owned QUST `AllowRepeatedStages` flag for a running quest.
Each authorized repeat re-evaluates its log conditions and executes eligible
fragments. Visited-stage history remains unique and the highest stage never
decreases. Each repeat advances the journal revision, including a fragment that
only adds an inventory reward without changing VM variables. A retry with the
previous `expectedRevision` is refused by the addon. Commands without a revision
are deliberate invocations, not automatically deduplicated network requests.

Sequential nested visits are supported. Reentering a still-active repeatable
stage requires the engine's asynchronous completion model and is refused with
rollback; it is never silently treated as an ordinary visited-stage no-op.
Stopped repeated visits require startup support and are refused. Unsupported
effects, stale contexts and later fragment failures roll back the revision and
all staged effects together. Ordinary state-only `questCommand` setters remain
idempotent and do not execute repeats.

Stage visitation precedes log conditions. The QSDT completion flag takes effect
before its fragment, so script queries observe completion in that transaction.
Completion does not stop the quest; Miscellaneous quests retain their active
flag, and completing an already-completed quest does nothing. A valid stage
without log items records visitation but returns false. Ambiguous duplicate
authored stage indices are refused instead of merging their log lists.

`QuestLifecycleData` owns and bounds-checks the exact DNAM flags, priority,
type and unknown bytes, ENAM event identifier, ordered stage indices/flags,
and separately scoped dialogue/story conditions. Stage/objective/alias
conditions cannot become quest-start conditions. Unknown metadata is retained
for future lifecycle gates. The owned catalog has 126 repeatable quest records;
this is metadata coverage, not 126 playable quests.

The lifecycle audit uses the owned 1.7.104 executable identified in
[CLIENT_RUNTIME.md](CLIENT_RUNTIME.md). Native Start can wait for startup and
accepts an already-running quest; its restart veto depends on RunOnce and the
instance history, not simply completion. Stop requests shutdown without waiting
for it. Reset can restart or shut down according to configuration. Non-RunOnce
startup resets quest and alias VM instances, and stage completion waits for log
callbacks. Those behaviors require durable lifecycle jobs, script continuations,
alias effects and correct event ordering. The isolated runner still rejects
Start/Stop/Reset; raw journal flags are not an implementation of these natives.

### Campaign forms and alias subset

The isolated runner represents static forms as `(campaign owner, original FormID)` and aliases as `(campaign owner, quest, alias ID)`. These are logical Papyrus objects, **not live NPC/reference clones**. `Game.GetPlayer` binds canonical PlayerRef to the campaign owner even when a guest triggers execution. `Game.GetForm`, `Form.GetFormID`, `ObjectReference.GetBaseObject` and `Actor.GetActorBase` read owned static identities; dynamic form lookup is rejected until campaign allocation exists.

Placed reference objects now bind their original base/reference scripts to the
campaign VM. Reference properties override base properties, with every object or
array element mapped through the plugin that supplied it. Both removed-script
flags and removed properties are honored. NPC `UseScript` inheritance follows
deterministic NPC templates. When inheritance reaches a leveled list, it uses
the bound campaign copy's persisted template selection; it never rerolls during
script loading. Missing selections, cycles and invalid bindings fail. Missing attached PEX is an error, including when encountered
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

Alias bindings, alias-script variables, globals and journal changes commit together with supported inventory and visibility effects. Failure before publication discards the staged effects together. Character reload retains bindings; quest reset removes that quest's bindings and script roots while preserving other quests and campaign globals. Broader live world-state transactions remain future work.

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

### Physical reference queries and visibility mutations

The isolated runner adds ten partial queries: `GetPositionX/Y/Z`,
`GetAngleX/Y/Z`, `Actor.IsDead`, `GetDistance`, `GetParentCell`, and
`GetWorldSpace`. Coordinates and angles come from authoritative server snapshots
(angles are degrees), not a client engine read. PlayerRef resolves to the
campaign owner; other physical references require an explicit private-copy
binding. Reads participate in the journal transaction and validate registered
lifetimes, phase epochs, saved state and binding identity before commit, even
when only the quest journal changes. They see staged inventory/visibility writes.

Distance requires the same interior or worldspace and finite, representable
coordinates. `GetParentCell` accepts a stored CELL; a WRLD-only exterior location
is refused until there is a campaign cell index. `GetWorldSpace` resolves a WRLD
or an exterior CELL's owned parent, and returns None for an interior. These are
native-method subsets; spatial condition functions, general spatial
queries, movement and AI behavior remain separate work.

`IsDisabled`, `Enable`, `Disable`, `EnableNoWait`, and `DisableNoWait` form the
five-method visibility subset. Mutations require a bound non-player copy in the
owner's private phase and `fade=false`. Enable-parent children and parents are
both rejected; propagation is not implemented. Occupied references and actors
with disable callbacks, pending snippets or respawn work are also rejected.
Supported calls stage state until commit; they do not invoke the ordinary
shared-reference Enable/Disable path or allow latent execution.

A net visibility change requires atomic storage and an installed projection
barrier. The native barrier captures every connected assigned actor in the
private phase, rejects legacy clients without packet epochs, and closes all
gameplay gates before calling the projection preparation hook. Every observer
must advance its token while connection identity, actor lifetime and phase epoch
remain unchanged. The journal, VM, inventory, visibility, grid membership, host
leases and pending save batch then publish together; subscription reconciliation
and fresh manifests/baselines follow while gameplay remains blocked. Publication
may rotate tokens again, or defer until client hello. Only a valid current-token
projection acknowledgement releases gameplay.

Every entered preparation has paired cleanup, including preparation exceptions.
An abort, reconciliation failure or failed publication receipt quarantines the
captured connections and attempts to close them. Delayed acknowledgements cannot
reopen a quarantined native gate; cleanup does not close a replacement connection
that reused a numeric user slot. After publication, failure is reported as
desynchronization rather than pretending committed state rolled back. Save ticks
still run asynchronously: these acknowledgements do not establish disk durability
(see [storage limits](STORAGE.md)).

A fresh enable can trigger first-subscription `OnLoad` while the outer visibility
barrier is still active. A visibility mutation in that nested load is refused;
the outer change has already committed and observers are quarantined. Deferring
or queuing that lifecycle work remains unimplemented. Fade, enable-parent
propagation, latent continuations, full event lifecycle and native multi-client
quest acceptance remain open. These partial methods enable no quest adapters and
certify no quests.

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
complete quest startup, combat events and ordinary player inventory events still
need implementation; no vanilla adapter is enabled.

### Private reference interaction events

Bound copies route native `OnActivate`, `OnTriggerEnter`, `OnTriggerLeave`,
`OnTrigger` and `SkympOnActivateClose` through the isolated campaign runtime.
The copy must already have completed reference initialization. Each event takes
one live native reference from the same private instance: a player participant
maps to the campaign's PlayerRef; a bound NPC or object maps to its original
form identity. Shared references, unbound objects, foreign phases, logical VM
objects supplied as native arguments, and unsupported event shapes are refused.

Attached reference scripts and matching scripted reference aliases of running,
adapter-enabled quests receive the event. Script variables, journal changes and
supported inventory effects (including resulting inventory-event handlers)
share one bounded transaction. A handler failure rolls those effects back.
The global VM remains inaccessible to bound copies. Queued trigger enter/leave
events retain both native lifetimes and instance epochs, so leaving and returning
or reusing an object ID cannot deliver a previous phase's event.

This is synchronous event ingress, not durable delivery or complete activation
semantics. Existing default activation processing, door teleports, activation
parents, container UI and other spatial effects are outside this transaction;
they still need campaign-specific integration. Movement is also outside the
script transaction. Automatic alias fills, remaining natives and latent handlers
remain restricted. Controlled script tests do not certify any vanilla quest or
establish throughput for 16Ã¢â‚¬â€œ32 players.

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
and optimization before a 16Ã¢â‚¬â€œ32 player rollout. Requests are bounded to 4,096
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

Protocol version 2 binds the manifest token to the native packet epoch and sends
a fresh world baseline as part of the same transition. Older manifest-only
handshakes are refused. The pure client replica validates and assembles both
parts, but acknowledges only when an engine adapter explicitly reports
application complete. The protocol smoke uses real native bot packets and
simulates that engine completion. It is not an in-game test. Incoming gameplay
gating does not freeze server simulation or outgoing replication. The Skyrim
engine adapter and client transition coordinator are now connected behind the
client's `campaign-projection: true` setting. General reference restoration and
quest execution remain unfinished. Keep projection unset in staging and normal
clients while native acceptance continues. No quests are certified.

### Packet epochs and world baselines (experimental server integration)

Trusted `mp.beginPacketEpoch(userId)` requires a connected user with gameplay
blocked. It creates a fresh, random 128-bit token; `mp.getPacketEpoch(userId)`
reads it. A new connection starts without a token and retains legacy transport.
The experimental projection feed now rotates the epoch at connection setup,
before changing a phase, and before capturing a replacement manifest/baseline.
That native token is also the acknowledgement token. An epoch changed by trusted
server code invalidates a pending acknowledgement even if the manifest matches.
Staging still has projection disabled; the client coordinator is opt-in and
requires `campaign-projection: true` in its settings.

Once selected, the native server requires the current token on all gameplay
input, including custom commands, even after the gameplay gate reopens. Outbound
replication carries the recipient's token. `CampaignFrame` (message type 34)
retains the original binary payload and reliability, adding 22 bytes. Frames are
bounded to a 1 MiB payload; nested, truncated and trailing frames are rejected.
Projection control stays unframed; initial login may pass before actor binding.
Deferred messages capture the enqueue-time token and are dropped after rotation
instead of being relabeled as current state. The fresh baseline replaces their
replicated state; executable snippets are never replayed by a baseline.

Trusted `mp.getWorldBaseline(userId, packetEpoch)` requires the current blocked
epoch and an active assigned character. It captures existing subscriptions,
using the same per-recipient spawn serializer and presentation/property filters
as normal stream-in. It returns gamemode definitions, the player first (including
explicitly empty inventory and a false creation flag), current visible reference
snapshots, and NPC hosting assignments. It does not reassign or respawn the actor,
change subscriptions, or rerun reference initialization/load handlers. References
stay alive during capture; changes to actor identity, phase, epoch, connection or
subscriptions invalidate it. The limit is 4,096 references and 2 MiB of serialized
data. This is a local visible-world snapshot, not every world record at once.

The common spawn serializer now always excludes `private.*` properties, even
when those fields were not registered in the gamemode property table. Previously
unregistered private fields could appear in initial spawn packets. Both normal
stream-in and the baseline now omit campaign VM state and allocation ledgers;
owner-only and neighbor-visible public fields retain their visibility rules.

The feed captures the complete baseline before publishing the begin message,
then transfers it in 16,384-character chunks tied to the same session, sequence
and epoch as the reference manifest. The client assembles it separately from
live replication. Missing, conflicting, malformed or cross-epoch chunks cannot
produce `prepared()` or an acknowledgement. The baseline validates one matching
self, unique reference identities, bounded counts, transforms, hosting targets
and public property envelopes; snippets and custom commands are forbidden.
The engine coordinator must replace the prior replica/hosting state, use the
self snapshot to bootstrap a generated world if needed, apply reference mapping,
and drain current-epoch deltas before gameplay resumes. Receipt alone does none
of that engine work.

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
gameplay. The client coordinator selects the validated epoch, cancels the prior
world, and waits for temporary-copy deletion, native calls, and original-NPC
visibility restoration before applying the new baseline. It suppresses mapped
originals, observes player/copy readiness, acknowledges, and waits for server
release. Queued deltas are applied and observed before player input resumes.
Journal subscriptions are replaced only after that release. General native
property restoration and quest execution still need implementation. Capture and publication are
currently synchronous and bounded; incremental streaming, loading deadlines and
16/32-client performance still require work. Tests exercise the native baseline,
transport and server/client protocol assembly, not Skyrim's engine or a playable
quest.

Verification: 83 Node tests, 219 native tests (90,853 assertions), server/client
TypeScript builds, native API smoke and projection/non-projection bot sessions.
Bot sessions cover gated login, native baseline capture, host/guest transition,
boundary departure, reconnect, stale/forged acknowledgements and unframed-input
rejection. Engine completion is simulated explicitly. Evidence is retained in
ignored `artifacts/world-baseline-*` locally and the corresponding
`/home/andrew/skymp-test-artifacts/world-baseline-*` logs on boyhouse.

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

The persisted field is `private.boyhouse.questJournal`. Unknown save versions are refused rather than reset. The implementation validates before writing and uses the existing character save path. The optional [SQLite driver](STORAGE.md) commits each complete save batch atomically across change forms. Supported inventory and visibility effects stage with journal/VM state, but broader world mutations and acknowledgements after disk commit remain unfinished. The ledger limit is 8 MiB. The diagnostic feed currently sends full snapshots, capped at 1 MiB; exceeding that limit returns unavailable rather than truncating progress. Delta/chunk transport is required before full campaign rollout. VM epochs are process-local guards, not network transition acknowledgements or VM persistence.

### Transactional factions

The isolated runner has partial `Actor.GetFactionRank`, `IsInFaction`,
`SetFactionRank`, `AddToFaction` and `RemoveFromFaction` implementations. Targets
are the campaign owner or a bound private actor. Reads use the transaction's
snapshot; changed factions publish and persist with journal, VM, inventory and
visibility effects. A failure before publication rolls back that complete set.
Notification failure after publication quarantines observers without replaying
already committed quest rewards.

Absent saved factions derive from immutable NPC/template metadata without
initializing or saving the live actor. A present snapshot, including an empty
one, is authoritative. Save encoding preserves signed byte ranks and accepts
the exact wrapped uint32 negative values written by older versions. Malformed
ranks, duplicate identities and snapshots above 4,096 entries are rejected.

Engine rank and membership are distinct. An absent faction returns rank -2;
an explicit removal override has rank -1. An active base membership survives
other negative reference overrides. A player-expelled base faction can therefore
have rank -2 while still returning true from `IsInFaction`. Network snapshots
carry `{factionId, rank, isMember}` so a client must prove both observations.
The raw SKSE `GetFactions` base/override union is not implemented by this API.

Unique actors, including the player, have an additional engine branch: removing
an existing base faction erases it instead of writing a reference tombstone.
The current reference-local projection cannot reproduce every resulting pair
of rank and membership. Existing unique-actor removals and rank writes below -1
are refused before committing effects. Full support requires isolated base-layer
state and its lifecycle; treating -1 as equivalent to absence would be wrong.
These restrictions are outstanding work, not completed faction compatibility.

Stage-log CTDA `GetInFaction` (71) and `GetFactionRank` (73) use the same scoped
reads. They accept explicit actor references and safely resolved reference
aliases, including alias zero. Static actor/faction record kinds are checked
before OR short-circuiting. Implicit physical subjects, unsupported flags and
non-stage contexts remain unimplemented. Owned MQ302 conditions exercise owner
selection in fixtures; those fixtures do not certify its campaign playthrough.

Faction publication closes the projection gate for all connected private-phase
members. In shared space it gates the owner and connected direct observers,
whose baselines contain the changed actor. Hosting an offline observer does not
itself subscribe a distant player. Observer identity, connection lifetime and
packet epochs are rechecked. Even an unchanged shared
world manifest needs a new baseline and acknowledgement before play resumes.

## Catalog-wide automation

The [dependency analyzer](DEPENDENCIES.md) discovers shared work across the full catalog; the [ranked backlog](QUEST_BACKLOG.md) records potential impact. It resolves load-order overrides, associates dialogue/scenes/story nodes, reads VMAD/PEX metadata, and traces typed calls to engine APIs. Unknown dependencies and runtime-dispatch candidates remain explicit. This automates discovery and regression inputs; it does not replace engine implementation or playthrough evidence.

## Remaining implementation gates

| Gate | Required work and evidence |
| --- | --- |
| Original quest execution | Extend the implemented scoped fragment runner with startup/shutdown scheduling, alias filling and alias scripts, nested quest lifecycle transitions, asynchronous stage completion, and the remaining condition/native semantics. The existing global VM must not become an alternate campaign-effect path. |
| VM persistence | Shared arrays, variables, globals, alias bindings and explicit nested script checkpoints persist. Add runner cursors, alias lifecycle, timers, content migrations and durable completion delivery. Restart during a stage/reward must neither lose nor duplicate effects. |
| World overlays | Explicit entry/recovery prepares private references and experimental projection replaces originals. Extend the transactional visibility subset with enable-parent propagation and lifecycle, plus death, inventory, globals, factions, locations, doors and encounters with their complete mutation/event semantics. Add automatic area entry and broader native acceptance. |
| Dialogue and story | Implement topic/response selection, conditions, aliases, scenes, packages, story-manager events, radiants, followers, and quest item/reward behavior. Every mutating effect must carry campaign context. |
| Client engine | Expand the experimental Skyrim Platform 1.7.104 port and the single-client projection round-trip evidence to new-character completion, general world restoration, multiple real clients and journal/objective markers without rerunning client fragments. See [the native runtime audit](CLIENT_RUNTIME.md). |
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

The repeat-stage/lifecycle-metadata batch passed the Linux server build and full
verification on 2026-11-17 UTC: 586 native cases / 424,152 assertions, 368
JavaScript tests, 47 Python tests, addon smoke, ordinary and projection protocol
smokes, and SQLite recovery of 32 characters. Regressions cover conditional
branch changes between repeats, repeated inventory rewards with unchanged VM
variables, guest isolation, restart, stale revisions and failing-fragment
rollback. The owned scan reports 2,274 quests, 14,026 PEX objects, 642 discovered
capabilities, no parse failures or explicit unresolved static dependencies, and
zero certified playable quests. Runtime-dependent dispatch remains on 1,146
quest records. The registry guards 127 source files. No Skyrim launch, client
installation update or staging deployment was part of this batch.

The subsequent leaf-checkpoint batch passed the Linux build and complete suite
on 2026-11-17 UTC: 610 native cases / 451,607 assertions, 368 JavaScript tests,
47 Python tests, addon and both protocol smokes, and the 32-character SQLite
restart check. Recovery tests destroy and reconstruct the VM twice, preserve
local-only/shared arrays and object declarations, route method/static/parent
results correctly, preserve budgets, and discard a failing resumed candidate
before retrying the original checkpoint. Ordinary promises and unsupported
nested suspension remain rejected. Owned archive scripts also pass executable
fingerprinting. The source registry now guards 131 files; catalog capability
counts and zero certified playable quests are unchanged. No game launch,
installation update or deployment was performed.

The nested-stack batch passed the Linux server build and full verification on
2026-11-17 UTC: 626 native cases / 453,419 assertions, 368 JavaScript tests,
47 Python tests, addon and both protocol smokes, and SQLite recovery of 32
characters. New cases restore nested methods, static helpers, Parent calls,
fallback-state handlers and property getters/setters through fresh VMs; retain
pending callers across further waits; and reject corrupted outer frames before
resumed leaf effects. Codec cases exercise schema 5 migration, stack/local
limits, shared caller-only arrays, and failed object resolution followed by a
clean retry. The owned scan remains at 2,274 quests, 14,026 PEX objects, 642
capabilities, 1,146 quests with runtime dispatch, no parse failures, and zero
certified playable quests. The registry still guards 131 source files. No
Skyrim launch, client installation change or staging deployment was performed.

The exact-save-receipt batch passed the Linux server build and full verification
on 2026-11-17 UTC: 649 native cases / 453,724 assertions, 368 JavaScript
tests, 47 Python tests, addon and both protocol smokes, and SQLite recovery of
32 characters. Receipt cases cover immutable FIFO retries, stale/duplicate
results, submission failures, admission bounds, backend replacement and abandoned
in-flight writes. A separate SQLite connection observes all receipt participants
before fresh-server recovery. World-reset tests prevent stale object indexes and
timers from affecting reused identities. The source registry guards
136 files; catalog counts and zero certified playable quests remain unchanged.
These are storage/runtime checks, not a quest playthrough. Production quests
still need persistent work cursors, outbox handling and lifecycle integration.
No Skyrim launch, client installation change or staging deployment was performed.
An initial JavaScript run terminated one test process with SIGSEGV; its 36 cases
passed in isolation and the full JavaScript suite passed on rerun without a
source change. That process crash was not reproduced or diagnosed.

The durable-stage-job batch passed the Linux server build and full verification
on 2026-11-17 UTC: 677 native cases / 452,979 assertions, 368 JavaScript
tests, 47 Python tests, addon and both protocol smokes, and SQLite recovery of
32 characters. The 28 focused cases (420 assertions) cover strict job/heap
validation, content changes, cumulative budgets, two nested waits with shared
arrays, saved log decisions, exact failed-save retries, duplicate commands and
completions, failed-resume rollback, synchronous inventory-event stage calls,
and fresh-server recovery at each wait and completion. MQ101/MQ102/DA04 metadata
with generated fragments exercises the shared runner; these are not quest
playthroughs. The owned scan remains at 2,274 quests, 14,026 PEX objects, 642
capabilities, 1,146 quests with runtime dispatch, no parse failures and zero
certified playable quests. The registry guards 145 sources. No Skyrim launch,
client installation change or staging deployment was performed.
