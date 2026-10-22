# MySQL Persistence

The server uses the MySQL C API through `Database`; `ChatStore` provides concrete
chat operations. MySQL 8.0 or later is required. Follow the [README](../README.md)
for installation, account creation, schema import, and startup.

## Schema

[sql/schema.sql](../sql/schema.sql) creates seven InnoDB tables with utf8mb4:

| Table | Purpose |
| --- | --- |
| `users` | Unique usernames and password hashes |
| `chat_groups` | Group names and owners |
| `group_members` | Memberships, including each owner |
| `messages` | Sender, optional client message ID, group or file, text, and creation time |
| `message_deliveries` | Recipients and acknowledgment timestamps |
| `files` | Owners, storage keys, original names, content types, and byte sizes |
| `file_recipients` | Download access for selected users |

IDs are unsigned 64-bit integers in MySQL and C++, represented as strings on the
wire. Usernames, client message IDs, and storage keys use a case-sensitive binary
collation. Connections use UTC timestamps. Foreign keys enforce references
between records.

File bytes live on disk. Online connection state lives in the Reactor rather
than a database flag that could become stale after a crash.

The schema script contains no database selection or credentials. Import it into
the selected database explicitly. `CREATE TABLE IF NOT EXISTS` permits repeated
imports but does not upgrade existing table definitions. The server checks
connectivity at startup; it does not create tables or run migrations.

## Upgrading an Existing Database

Databases created before file transfers need
[001_file_messages.sql](../sql/migrations/001_file_messages.sql). Apply it once,
before starting the updated server:

```bash
mysql --protocol=TCP -h 127.0.0.1 -u chatroom -p chatroom \
  < sql/migrations/001_file_messages.sql
```

The migration adds `messages.file_id` and its foreign key and permits NULL client
message IDs for server-created file notices. Existing messages are preserved.
Apply it separately to `chatroom_test` if that database also has the old schema.
New databases need only the current `sql/schema.sql`.

## Connection Ownership

Call `initialize_mysql()` before starting workers. Thread-local RAII initializes
and releases each calling thread's MySQL state; library cleanup follows worker
shutdown.

A `Database` owns one connection and stays on its creating thread. A
`ChatStore` borrows that connection and must not outlive it. TCP and HTTP workers
lazily create and reuse their own connections. A database error discards the
affected connection; a later request can create another. There is no shared
connection pool or transparent transaction retry.

The startup connectivity check closes its connection before workers start.
Blocking SQL runs on workers. Connect, read, and write timeouts are five seconds;
they are socket timeouts, not an overall query deadline.

## Queries and Transactions

`execute()` and `query()` use prepared statements. Application SQL and bound
string, unsigned integer, and NULL parameters remain separate. Buffered results
use optional strings, preserving SQL NULL, empty strings, embedded zero bytes,
and long values. Queries should select explicit columns and bound their results.

`DatabaseError` exposes the MySQL error number without SQL values or credentials.
`Transaction` rolls back unless `commit()` succeeds. Transactions cannot be
nested; store methods that write multiple tables own their transaction.

## Store Contracts

| Operation | Behavior |
| --- | --- |
| `create_user()` | Stores an already hashed password; duplicate usernames raise MySQL error 1062 |
| User lookups | Return `std::nullopt` for a missing user |
| `existing_user_ids()` | One prepared `SELECT id ... IN (...)`; sorted unique results, empty input allowed |
| `create_group()` | Atomically stores the group and deduplicated members, including its owner |
| `groups_for_user()` | Reads ID, owner, and name after an exclusive group-ID cursor |
| `save_direct_message()` | Saves one message and deduplicated recipient deliveries atomically |
| `save_group_message()` | Checks membership, locks a membership snapshot, and saves other members' deliveries |
| `pending_messages()` | Reads oldest unacknowledged deliveries without changing their state |
| `acknowledge_message()` | Confirms only the specified recipient's delivery; repeats preserve the first timestamp |
| `create_file()` | Atomically stores metadata, permissions, and notification message/deliveries |
| `file_for_user()` | Returns metadata only to the owner or a selected recipient |

Application services validate field lengths and permissions. Recipient checks use
one ID-only query; foreign keys remain the final consistency check. Group listing
reads at most 65 rows to return a 64-entry page without per-group member queries.

The unique `(sender_id, client_message_id)` key identifies an immutable text
message. An upsert serializes concurrent attempts. Exact retries return the
original ID without creating deliveries; changed text, message kind, group, or
direct recipients raise `std::invalid_argument`. Group retries retain the
original recipient snapshot. A failed insert rolls back the entire send.
Deadlocks and transient database failures become request errors; writes are not
automatically repeated.

Acknowledged delivery rows remain stored, so a sender retry cannot recreate them.
Replay queries select pending rows anew instead of keeping a highest-ID cursor,
which could skip a lower ID committed later. `pending_messages()` defaults to a
100-row caller limit; the dispatcher uses tighter count and serialized-byte
limits. File metadata is joined in the same query. See
[Messaging](messaging.md#acknowledgments-and-offline-replay).

## Storage and Recovery

Keep MySQL data and the configured file directory across server restarts. Back
them up together. Database transactions do not include filesystem writes; crash
boundaries and orphan-file behavior are described in
[Files](files.md#notifications-and-persistence).

## Integration Tests

Use only the disposable `chatroom_test` database. Catch2 fixtures import the
schema and delete its records before each case; they refuse other database names.
CTest runs database cases serially, but separate test processes must also be run
one at a time. Full setup and commands are in [Testing](testing.md#mysql-tests).
