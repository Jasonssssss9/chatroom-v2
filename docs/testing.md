# Integration Verification

The ordinary Catch2 suite needs no database. MySQL integration tests and the
process-level walkthrough use only the disposable `chatroom_test` database.
Never run two database test processes at the same time.

## Catch2 and CTest

Build and run the ordinary suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

After creating the test database as described in [Database](database.md#integration-tests),
enable database tests and supply credentials through the current shell:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DATABASE_TESTS=ON
cmake --build build -j2
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_DB_NAME=chatroom_test
ctest --test-dir build --output-on-failure
```

Each database fixture deletes test data before running. Do not point a development
server at this database during testing. To select file tests directly:

```bash
./build/tests/chatroom_tests '[file]'
./build/tests/chatroom_database_tests '[file]'
```

File tests cover binary and empty files, exact size limits, metadata boundaries,
access permissions, incomplete uploads, database rollback, unavailable storage,
stored-size mismatches, and recovery after storage errors. They also verify that
a failure after commit keeps the file and its pending notification. Client tests
cover bounded responses, malformed replies, authentication errors, cancellation,
partial-download cleanup, retries after cancellation, and local overwrite prevention.

## Real Server and Client Processes

The optional Python 3 script uses only the standard library. It starts the built
C++ executables on available loopback ports and talks to the real MySQL database.
It does not replace the server or client with mocks.

With the database environment above configured, run:

```bash
python3 tests/end_to_end.py --build-dir build
```

The script runs the same workflow twice, once with a normal `SIGTERM` shutdown
and once with `SIGKILL`. Each workflow restarts the server with the same database,
storage directory, and JWT secret. It verifies:

| Workflow | Checks |
| --- | --- |
| Authentication | HTTP registration and login, duplicate names, incorrect passwords, TCP JWT authentication, and console registration and login |
| Private and multi-recipient chat | Online delivery, repeated recipients, an offline recipient, exact retries, conflicting retries, and repeated ACKs |
| Groups | Creation, member lists, group discovery, broadcasts, nonmember rejection, and retry keys after restart |
| Files | Binary upload and download, owner and recipient access, anonymous and outsider rejection, and real console upload and download |
| Reconnection | Disconnect before file ACK, replay on the next connection, automatic console ACKs, and no replay after confirmation |
| Restart recovery | Users, groups, messages, retry keys, pending text/file deliveries, stored bytes, and download permissions survive both shutdown modes |
| Client shutdown | An active console displays server disconnection and exits; a new console logs in after the restart |

The script creates uniquely named test users and records in `chatroom_test`; it
does not delete existing records. It removes its temporary storage directory when
finished. A later Catch2 database fixture clears these test records. The script
generates its own temporary JWT secret, so it does not need `CHATROOM_JWT_SECRET`.

Text retries reuse the original message ID when the sender, client message ID,
content, and recipients match. Uploads have no idempotency key: submitting the
same file again creates a distinct file and notification. ACK confirms acceptance
of a notification, not completion of a file download. These behaviors are checked
explicitly rather than treated as duplicate-delivery failures.

## Standalone Client

The client build has no server, MySQL, or JWT implementation dependency:

```bash
cmake -S . -B build-client -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF -DBUILD_TESTING=ON -DBUILD_DATABASE_TESTS=OFF
cmake --build build-client -j2
ctest --test-dir build-client --output-on-failure
python3 tests/end_to_end.py --build-dir build --client-build-dir build-client
```

## Sanitizers

Use a separate build for AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_DATABASE_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build-sanitized -j2
ctest --test-dir build-sanitized --output-on-failure
python3 tests/end_to_end.py --build-dir build-sanitized
```

Run this after other database tests finish. Sanitizers and these functional tests
do not establish a throughput or simultaneous-connection benchmark. `SIGKILL`
checks process termination after successful commits; it does not simulate a power
failure of MySQL and the filesystem. See [Files](files.md#notifications-and-persistence)
for the boundary between database transactions and disk storage.

## Verification Snapshot: October 21, 2026

| Configuration | Result |
| --- | --- |
| Full Debug build | 183 Catch2 cases passed, including 46 MySQL integration cases |
| AddressSanitizer and UndefinedBehaviorSanitizer | All 183 cases passed, with no sanitizer diagnostics in the test log |
| Standalone Debug client | All 57 cases passed |
| Fresh standalone Release client | All 57 cases passed; the executable links no MySQL or OpenSSL libraries |
| Debug server with standalone Release client | The full process walkthrough passed with both `SIGTERM` and `SIGKILL` |
| Sanitized server and client | The full process walkthrough passed with both `SIGTERM` and `SIGKILL` |

The review followed framing and JSON validation, socket and connection ownership,
worker handoff and completion ordering, session replacement and expiry, prepared
statements and transactions, notification replay, disk-file ownership, client
cancellation, configuration, logging, and build dependencies. The current module
boundaries support these workflows without additional application abstractions.
This pass added five Catch2 cases, the process walkthrough, and documentation;
it required no application or protocol changes.
