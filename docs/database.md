# MySQL Persistence

The server uses the MySQL C API through `Database`, with concrete chat operations
in `ChatStore`. MySQL 8.0 or later is required. The server and database can run in
the same WSL Ubuntu instance.

## Setup

On Ubuntu, install the service, command-line client, and development files:

```bash
sudo apt update
sudo apt install mysql-server mysql-client libmysqlclient-dev pkg-config
sudo systemctl enable --now mysql
```

Create the development database and account through `sudo mysql`:

```sql
CREATE DATABASE chatroom CHARACTER SET utf8mb4;
CREATE USER 'chatroom'@'127.0.0.1' IDENTIFIED BY 'replace-with-your-own-password';
GRANT ALL PRIVILEGES ON chatroom.* TO 'chatroom'@'127.0.0.1';
```

Import the schema from the repository root. The client prompts for the password:

```bash
mysql --protocol=TCP -h 127.0.0.1 -u chatroom -p chatroom < sql/schema.sql
```

The script selects no database and contains no credentials. It can be imported
into either `chatroom` or `chatroom_test`. `CREATE TABLE IF NOT EXISTS` allows
reimporting it but does not upgrade existing table definitions. Schema changes
must be applied explicitly; there is no automatic migration system.

Supply the password and start the server:

```bash
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_JWT_SECRET="$(openssl rand -hex 32)"
./build/server/chatroom_server
```

Other settings use the existing JSON configuration and `CHATROOM_DB_*` environment
variables. See [Configuration](configuration.md). The executable checks database
connectivity before starting workers and the Reactor, logs `database_connected`,
then closes that startup connection. A failed connection stops startup. It does
not create databases or tables. `.env` files are not loaded automatically.

## Schema

All seven tables use InnoDB and utf8mb4. IDs are unsigned 64-bit integers in C++
and will be strings in protocol messages. Usernames, client message IDs, and
storage keys use a case-sensitive binary collation. Connections use UTC timestamps.

| Table | Purpose |
| --- | --- |
| `users` | Unique usernames and password hashes |
| `chat_groups` | Group name and owner |
| `group_members` | Membership, including the owner |
| `messages` | Sender, client message ID, optional group, text, and creation time |
| `message_deliveries` | Recipient and acknowledgment time for each message |
| `files` | Owner, storage key, original name, MIME type, and byte size |
| `file_recipients` | File access for selected users in addition to the owner |

Foreign keys reject missing users and groups. File contents remain on disk. The
future upload service generates storage keys; they are not client-supplied paths.
Online connection state belongs to the Reactor rather than a persisted boolean
that could become stale after a crash.

## Connections and Queries

Call `initialize_mysql()` before starting workers. `Database` also initializes
the calling thread's MySQL state. Thread-local RAII releases it when that thread
exits; library cleanup happens after threads finish.

A `Database` owns one connection and must stay on its creating thread. A
`ChatStore` borrows it and must not outlive it. Different workers use different
connections. HTTP authentication and TCP handlers reuse a private connection on
each worker; a database error discards that connection. This module does not introduce
a shared connection pool. Blocking database work
belongs on workers. The startup check runs before the event loop starts.

`execute()` and `query()` use prepared statements. SQL text comes from application
code; string, unsigned integer, and SQL NULL parameters are bound separately.
Buffered results are rows of optional strings, preserving NULL, empty strings,
embedded zero bytes, and long values. Use explicit columns and bounded results.

Connection, read, and write timeouts are five seconds. These are socket timeouts,
not an overall query deadline. `DatabaseError` exposes the MySQL error number.
Its message contains the failing operation and code rather than SQL values or
credentials. There is no transparent reconnect that could lose transaction state.

`Transaction` rolls back unless `commit()` succeeds. Transactions must not be
nested. Store methods that write multiple tables already own their transaction.

## Store Behavior

`create_user()` accepts an already hashed password. The HTTP authentication service
provides validation, hashing, and verification. Missing users return
`std::nullopt`; a duplicate username raises MySQL error 1062.

`existing_user_ids()` selects only IDs using one prepared `SELECT ... IN (...)`
query, with a placeholder for each supplied ID. It returns the matching IDs in
ascending order without duplicates; an empty input returns an empty result.
`send_message` compares this result with its sorted, deduplicated recipients
before saving. Foreign keys remain the final consistency check.

`create_group()` inserts the group and members atomically, always including the
owner. Repeated member IDs are collapsed. `save_group_message()` requires the
sender to be a member and locks a membership snapshot. It returns `std::nullopt`
when the sender is not a member; success returns the message ID and persisted
recipient IDs, including on retries. Every other member gets a delivery. A group
containing only its sender can persist a message without deliveries. The TCP
handler bounds groups to 64 members and uses the returned delivery snapshot for
broadcast. See [Group Chat](groups.md).

Direct and group sends save the message and recipients in one transaction. A
failed recipient insert rolls back the whole operation. Direct sends require at
least one recipient and collapse duplicate recipient IDs.

The unique `(sender_id, client_message_id)` key identifies an immutable outgoing
message. An upsert serializes concurrent attempts on that key. Exact retries
return the original ID without creating deliveries. Changed text, group, or
direct recipients raise `std::invalid_argument`. Group retries keep their original
recipient snapshot even if members have joined since then. Deadlocks and other
transient failures become request errors; arbitrary writes are not retried
automatically.

`pending_messages()` returns the oldest unacknowledged messages, with a caller
limit of 100 by default. It does not mark them delivered. After acknowledgment,
the next batch excludes them. There is no persistent last-ID cursor that could
skip a lower ID committed by another transaction later. Callers must also respect
the protocol's byte limit when forming response batches.

`acknowledge_message()` affects only the specified recipient. Repeated
acknowledgments succeed without changing the original timestamp. Another user's
delivery cannot be acknowledged. Retaining acknowledged records prevents sender
retries from recreating deliveries.

`create_file()` saves metadata and allowed recipients atomically.
`file_for_user()` returns metadata only to the owner or a selected recipient.
Upload/download I/O and file notifications remain service work.

Field lengths and action-specific validation belong to those services; database
constraints provide the final consistency check. HTTP registration and login now
use the user operations. TCP authentication verifies the token's user through
`user_by_id()`. `send_message` validates recipients and calls `save_direct_message()`
before returning online notifications. A successful socket send leaves a delivery
unacknowledged. TCP `ack_message` records the authenticated recipient's confirmation;
authentication and completed replay batches trigger bounded queries for remaining
unacknowledged messages. See [Messaging](messaging.md).

## Integration Tests

Ordinary tests do not require a running database. Enable the separate integration
test executable explicitly:

```bash
cmake -S . -B build -DBUILD_DATABASE_TESTS=ON
cmake --build build -j2
```

Create a disposable test database through `sudo mysql`:

```sql
CREATE DATABASE chatroom_test CHARACTER SET utf8mb4;
GRANT ALL PRIVILEGES ON chatroom_test.* TO 'chatroom'@'127.0.0.1';
```

Use the same password environment variable and select this database:

```bash
CHATROOM_DB_NAME=chatroom_test ctest --test-dir build -L database --output-on-failure
```

Tests refuse to run against any database name other than `chatroom_test`. Each
fixture imports `sql/schema.sql` and deletes data in dependency order. CTest runs
these cases serially. Do not run a second test process against the same database
at the same time. Test records may remain afterward; keep this database disposable.

Coverage includes parameter binding, Unicode and binary-safe strings, rollback,
duplicate usernames, bounded batches, acknowledgments, conflicting retries,
group authorization, file access, and concurrent message deduplication.
