# Transactional campaign storage

The `sqlite` database driver persists a whole `IDatabase::Upsert` batch in one
transaction. A failed row, duplicate form identity, serialization error or commit
failure rolls back the batch. The world retains an immutable snapshot for retry
and receives a result tied to that submission; legacy callers can still use the
`UpsertFailedException` path. Empty slots in the world save buffer are ignored.
A successful return reports only committed rows.

Configure the server with:

```json
{
  "databaseDriver": "sqlite",
  "databaseName": "./world.sqlite3"
}
```

The default filename for this driver is `world.sqlite3`. The general server
default remains the legacy `file` driver; newly initialized Boyhouse laboratory
settings explicitly choose SQLite. Initialization never replaces existing settings.

## Contract and limits

- SQLite is pinned through the repository's vcpkg checkout to 3.52.0. The driver
  checks a minimum runtime/header version of 3.51.3, which includes the upstream
  [WAL-reset correction](https://www.sqlite.org/wal.html).
- WAL and `synchronous=FULL` are required, with `fullfsync` enabled. Durability
  depends on the operating system and storage honoring synchronization requests;
  see SQLite's [synchronization contract](https://www.sqlite.org/pragma.html#pragma_synchronous).
  Use local storage on the server, not a network filesystem.
- Each `Iterate` uses one read transaction, including filtered reads that execute
  several SQL statements. Concurrent connections see committed snapshots.
- The schema uses application ID `0x534b4d50`, version 1, and a `change_forms`
  table keyed by stable `FormDesc`. Only an entirely empty database is initialized.
  Foreign databases and unsupported schema versions are refused without rewriting
  their data. Invalid JSON, wrong column types and mismatched saved identities
  throw; they are not skipped. Each serialized change form is limited to 32 MiB.
- `AsyncSaveStorage` serializes calls to one driver. Independent reader connections
  are supported. A single driver object is not a public concurrent/reentrant API.
- The scoped fragment runner stages journal/VM state, supported inventory and
  private-copy visibility changes together. It validates participants and read
  snapshots, prepares the pending save buffer and affected grid/host state, then
  publishes with non-allocating swaps. An attached storage backend must advertise
  atomic batches; SQLite does, legacy drivers do not. Visibility also requires
  native observer gates and fresh packet epochs before publication. Subscription
  reconciliation, inventory notifications and replacement projections follow
  the in-memory commit; failed delivery quarantines observers without retrying
  the committed rewards as though the transaction had rolled back.
- `QuestJournal::AtomicDurable` seals an immutable owner/effect batch and returns
  a server-thread `SaveReceipt`. It requires atomic storage with per-submission
  results; journal-only and no-op calls also save the owner. `Pending` becomes
  `Durable` only when a matching successful result is processed by a world tick.
  A sequence, campaign identity and journal revision identify the receipt within
  this world lifetime. `GetNumFinishedUpserts` is a callback counter, not this
  durability signal.
- Sealed batches drain in FIFO order. Failed attempts retry the retained forms,
  including removed or replaced actors, before any later save; they never rebuild
  the failed snapshot from current live state. Ordinary updates continue to
  coalesce behind sealed batches. Stale/duplicate results and unrelated read or
  recycling errors cannot release the write barrier. Queue admission allows at
  most 256 sealed batches and 128 MiB of receipt payload accounting (serialized
  forms plus sparse-vector slots); refusal precedes transaction publication.
- Backend replacement is refused while writes are pending or a transaction is
  active. `Clear`/destruction marks pending receipts `Abandoned`: an already
  submitted write may still commit. A reused world drains that attempt before
  writing newer state, without retrying the abandoned batch. Receipts are
  process-local observations; persistent job markers and an outbox are still
  required to recover a commit whose acknowledgment was lost.
- Quiescent world clearing also resets object/profile/private indexes, hosting
  leases, deferred form caches and the shared VM/policy, and cancels regular,
  effect and reloot timers. Old timer callbacks and indexed pointers must not
  target newly created actors when identifiers are reused.
- A world save tick still submits batches asynchronously. Client notifications
  do **not** wait for disk commit, so a crash can lose a recently acknowledged
  change. Persistent execution of external events, complete world-effect staging
  and integration of job dispatch with durable receipts remain required.
  Switching drivers alone does not make arbitrary world-effect natives safe to
  enable.
- VM schema 6 can store a nested script checkpoint and preserve arrays held only
  by any saved caller's locals. Its codec consumes a pending completion token once within the
  candidate heap. The caller still has to commit token consumption, resumed
  effects and the next checkpoint together, and wait for storage acknowledgement
  before acknowledging durable delivery. JSON/fresh-VM recovery tests do not
  establish that crash boundary. Production stage/event execution remains
  synchronous through its existing API. The experimental `QuestJobRunner`
  persists stage/log work and gates its wait/result observations on receipts;
  real latent natives, event cursors and lifecycle jobs remain unfinished.
- Change forms now store maximum attributes and regeneration values as well as
  percentages. Legacy saves without the optional `actorValues` object retain
  the previous default attributes; missing members in a present object are an
  error. Explicitly empty faction lists are saved, so removing every faction
  remains distinct from never initializing factions. The ordinary shared-world
  actor loader still applies its existing attribute recalculation; private
  campaign copies use direct state restoration.
- Private copies restore without shared-VM load events or implicit corpse
  respawns. A persisted reference initialization marker prevents repeat `OnInit`
  rewards. Effect and loot timer recovery is still unsupported for those copies
  and fails explicitly. These changes do not make world timers durable.

## Existing saves and backups

Changing `databaseDriver` does not import an old file/ZIP/Mongo database. Stop the
server and back up the entire private state directory before any conversion.
Preserve the old database until every source form and its payload have been
validated against a reopened destination. The legacy migration driver is not an
audited all-or-nothing migration tool: it uses chunks and its file reader can
skip invalid source files. Do not use it as proof of a complete conversion.

For backups, stop every process that writes the database and copy the state
directory, including any `world.sqlite3-wal` and `world.sqlite3-shm` files. Do not
copy only the main file while it is in use: committed transactions may still be
in the WAL. A live backup must use SQLite's backup API or another documented
consistent snapshot mechanism. The fork does not yet expose a live backup API.

The Boyhouse laboratory's old `world/changeForms` directory was inspected and
contained zero files before switching its configuration. Its pre-switch state is
backed up privately and the old directory is retained. No player saves were
converted or discarded.

## Verification

`unit/SqliteDatabaseTest.cpp` exercises the actual driver with isolated temporary
files: complete change-form round trips, filtering/recycled buffers, failed
middle-row rollback, retries, duplicate identities, concurrent snapshots,
foreign/versioned databases, corrupt payloads and the asynchronous world save
path. Linux tests kill child processes before and after commit and reopen through
the driver. Abrupt process death is tested; hardware power-loss testing is not.

The campaign transaction regression transfers inventory between a character and
an explicitly bound private NPC, changes the journal/VM state, waits for the real
asynchronous SQLite save, then loads a fresh server and checks all participants
and the persisted reference binding. Separate transaction tests check stale
identities, phase changes, conflicting writes, failed handlers and non-atomic
storage. This does not certify general world effects or native-client behavior.

Receipt regressions delay and fail individual submissions, replay stale results,
throw after consuming submission arguments, and remove/reuse actors between
attempts. The SQLite receipt case waits for the exact receipt, reads complete
owner/NPC rows through an independent connection, and reloads a fresh server.
World-reset cases check abandoned write barriers, cleared indexes and cancelled
timers. These checks do not yet exercise a persisted quest job or outbox.

The private-copy restart regression saves a dead story NPC with changed
attributes, equipment and an empty faction list, plus a placed ammunition stack,
then loads them through a fresh server and the actual SQLite driver. Malformed
copy identities and unsupported pending timers are rejected before registration.
Scoped load tests also exercise failed-handler rollback, initialization retry,
owner/guest isolation, and loading a copy before its owner.

`node misc/boyhouse/storage-smoke.cjs` uses the native addon and owned masters.
It waits for 32 character saves to be visible in the database, kills the writer,
starts a fresh server, compares private journals/VM state and inventories, and
checks that party recovery restores a guest's personal history. Data is disposable
and adapters are enabled only inside this test. It does not simulate 32 Skyrim
clients, certify gameplay performance, or complete any quest.

The regular Boyhouse verification script includes both the database unit cases
and this restart smoke. No vanilla quest adapter is enabled by this work.
