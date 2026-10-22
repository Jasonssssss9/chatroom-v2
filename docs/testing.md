# Testing

The ordinary Catch2 suite needs no running database. MySQL tests, process
integration, and benchmarks use only the disposable `chatroom_test` database.
Run database-backed tools one at a time, with no development server using that
database.

## Ordinary Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DATABASE_TESTS=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The suite covers framing, protocol validation, socket I/O, worker ordering,
sessions, configuration, logs, password hashing, JWTs, and console behavior.
Select a feature directly with Catch2 tags:

```bash
./build/tests/chatroom_tests '[file]'
./build/tests/chatroom_tests '[protocol]'
```

## MySQL Tests

First create the database account described in the [README](../README.md).
Then create a separate test database and grant that account access:

```bash
sudo mysql <<'SQL'
CREATE DATABASE IF NOT EXISTS chatroom_test CHARACTER SET utf8mb4;
GRANT ALL PRIVILEGES ON chatroom_test.* TO 'chatroom'@'127.0.0.1';
SQL

mysql --protocol=TCP -h 127.0.0.1 -u chatroom -p chatroom_test < sql/schema.sql
export CHATROOM_DB_HOST=127.0.0.1
export CHATROOM_DB_PORT=3306
export CHATROOM_DB_NAME=chatroom_test
export CHATROOM_DB_USER=chatroom
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
```

These environment settings apply to the current terminal. Keep
`CHATROOM_DB_NAME=chatroom_test` for all commands below. Existing test databases
with an older schema need the [migration](database.md#upgrading-an-existing-database).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DATABASE_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

This runs both suites. To run only MySQL cases, use
`ctest --test-dir build -L database --output-on-failure`.
Fixtures require the exact database name `chatroom_test`, import the schema,
and delete records in dependency order before each case. Test data may remain
afterward. Do not use this database for anything you want to keep.

Database cases exercise prepared statements, transactions, authentication,
private and group messages, retries, permissions, ACKs, replay, and file storage.
File coverage includes binary and empty files, size and metadata limits,
incomplete uploads, rollback cleanup, missing storage, size mismatches, and
preservation after a post-commit failure. Client cases cover streaming,
cancellation, partial-file cleanup, malformed responses, and overwrite prevention.

```bash
./build/tests/chatroom_database_tests '[file]'
```

## Real Server and Client Processes

With the test schema and environment above, run:

```bash
python3 tests/end_to_end.py --build-dir build
```

Python 3 is required. The script uses only Python's standard library;
no pip packages or virtual environment are required.
It starts the actual C++ executables on available loopback ports.

Each workflow runs once with SIGTERM and once with SIGKILL, restarting with the
same database, file directory, and JWT secret.

| Workflow | Checks |
| --- | --- |
| Authentication | Registration, duplicate names, login, incorrect passwords, and TCP JWT validation |
| Private and multi-recipient chat | Online/offline recipients, duplicate recipients, exact and conflicting retries, repeated ACKs |
| Groups | Creation, discovery, member queries, broadcast, nonmember rejection, retry keys after restart |
| Files | Binary bytes, owner/recipient access, anonymous/outsider rejection, and console transfers |
| Reconnection | Replay after disconnect before ACK, automatic ACKs, and no replay after confirmation |
| Restart recovery | Users, groups, retry keys, pending text/file deliveries, bytes, and permissions |
| Console lifecycle | Disconnect notice and exit, then login through a newly started client |

The script creates unique records without deleting existing records. Its temporary
file directory is removed afterward; test database rows may remain until a later
Catch2 fixture clears them. It generates its own temporary JWT secret and does
not require `CHATROOM_JWT_SECRET`.

Exact text retries reuse the message ID; a repeated upload creates another file.
ACK confirms notification acceptance, not file download. The script checks these
contracts explicitly. SIGKILL exercises application-process recovery after
committed writes; it does not simulate a MySQL or filesystem power failure.

## Standalone Client

Build and test the client independently of MySQL, OpenSSL, and server code:

```bash
cmake -S . -B build-client -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF -DBUILD_TESTING=ON -DBUILD_DATABASE_TESTS=OFF
cmake --build build-client -j2
ctest --test-dir build-client --output-on-failure
python3 tests/end_to_end.py --build-dir build --client-build-dir build-client
```

The last command still needs the full server build and test database. To build
only the client executable without Catch2, set `BUILD_TESTING=OFF`.

## Sanitizers

Use a separate build with the same test database environment:

```bash
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_DATABASE_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build-sanitized -j2
ctest --test-dir build-sanitized --output-on-failure
python3 tests/end_to_end.py --build-dir build-sanitized
```

Functional tests and sanitizer runs do not measure concurrent capacity. See
[Performance](performance.md) for Release benchmarks and
[Files](files.md#notifications-and-persistence) for disk/database crash boundaries.

## Verification Snapshot: October 21, 2026

| Configuration | Result |
| --- | --- |
| Full Debug build | 183 Catch2 cases passed, including 46 MySQL cases |
| AddressSanitizer and UndefinedBehaviorSanitizer | All 183 cases passed with no sanitizer diagnostics in the test log |
| Standalone Debug client | All 57 cases passed |
| Fresh standalone Release client | All 57 cases passed; no MySQL or OpenSSL linkage |
| Debug server with standalone Release client | Process integration passed with SIGTERM and SIGKILL |
| Sanitized server and client | Process integration passed with SIGTERM and SIGKILL |

These are recorded results for the tested builds and workflows. Benchmark
measurements are recorded separately in [Performance](performance.md).
