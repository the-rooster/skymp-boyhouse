# Transactional campaign storage

The `sqlite` database driver persists a whole `IDatabase::Upsert` batch in one
transaction. A failed row, duplicate form identity, serialization error or commit
failure rolls back the batch and preserves its change forms in the existing
`UpsertFailedException` retry path. Empty slots in the world save buffer are
ignored. A successful return reports only committed rows.

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
- The scoped fragment runner stages journal/VM state and supported inventory
  changes together. It validates every participant, prepares the complete pending
  save buffer, then publishes live inventory/journal fields with non-allocating
  swaps. An attached storage backend must advertise atomic batches; SQLite does,
  legacy drivers do not. Inventory notifications follow this in-memory commit.
- A world save tick still submits the batch asynchronously. Client notifications
  do **not** wait for disk commit, so a crash can lose a recently acknowledged
  change. Persistent execution of external events, complete world-effect staging
  and durable acknowledgements remain required. Switching drivers alone does not
  make arbitrary world-effect natives safe to enable.

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

`node misc/boyhouse/storage-smoke.cjs` uses the native addon and owned masters.
It waits for 32 character saves to be visible in the database, kills the writer,
starts a fresh server, compares private journals/VM state and inventories, and
checks that party recovery restores a guest's personal history. Data is disposable
and adapters are enabled only inside this test. It does not simulate 32 Skyrim
clients, certify gameplay performance, or complete any quest.

The regular Boyhouse verification script includes both the database unit cases
and this restart smoke. No vanilla quest adapter is enabled by this work.
