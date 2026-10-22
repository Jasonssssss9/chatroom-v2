# chatroom-server-v2

A chat server and command-line client for Linux, written in C++17.

The server uses non-blocking TCP, epoll, and the Reactor pattern for real-time
chat, HTTP and JWT for authentication, and MySQL for users, groups, and messages.
Files are stored on disk, with metadata and download permissions in MySQL.

The server and client network layers are implemented. The server accepts JSON
requests for ping, authentication, private and group chat, member queries, and
acknowledgments, and returns structured errors. The command-line client provides registration, login,
and chat commands, with raw JSON available for debugging.
Application requests run in order for each connection on a configurable worker
pool (four threads by default). Their results return to the Reactor for socket output.
JSON configuration, command-line overrides, environment-based credentials, and
structured server logs are implemented.
MySQL connections, prepared statements, transactions, and persistence for users,
groups, messages, acknowledgments, and file metadata are implemented. The server
checks database connectivity at startup. HTTP registration, login, password hashing,
and JWT issuance are implemented. TCP verifies JWTs and binds users to connections,
with one online connection per user. Private messages and multi-recipient sends
are persisted before online notifications. Clients acknowledge deliveries, and
authentication triggers bounded replay of unacknowledged messages. Groups support
creation, paginated group listings, member queries, and persisted broadcast.
HTTP file uploads and downloads are streamed, with authenticated access and
ACK-able online and offline notifications.

## Project Layout

```text
server/
    CMakeLists.txt   Server build configuration
    src/            Server source code
client/
    CMakeLists.txt   Client build configuration
    src/            Command-line client source code
common/             Protocol, framing, logging, and small resource utilities
tests/              Catch2 tests
cmake/              Shared third-party dependency configuration
config/             Example configuration files
sql/                Database schema scripts
docs/               Design and usage documentation
```

See [Architecture](docs/architecture.md) for the design and project conventions,
[Networking](docs/networking.md) for the network API, and [JSON Protocol](docs/protocol.md)
for request fields, response examples, errors, and the mapping from v1.
The [threading guide](docs/threading.md) explains task queues, ownership, ordering,
and shutdown with active work.
See [Configuration and Logging](docs/configuration.md) for settings, environment
variables, startup options, and log fields.
See [MySQL Persistence](docs/database.md) for setup, schema, database operations,
connection ownership, and integration tests.
See [Authentication](docs/authentication.md) for registration, login, JWTs,
TCP sessions, HTTP settings, and examples.
See [Messaging](docs/messaging.md) to chat between two command-line clients and
understand persistence, retries, and online delivery.
See [Command-Line Client](docs/client.md) for commands and display options.
See [Group Chat](docs/groups.md) for group commands, permissions, and delivery snapshots.
See [File Transfers](docs/files.md) for storage configuration, schema migration,
HTTP endpoints, console commands, and notification delivery.
See [Connection Keepalive](docs/keepalive.md) for automatic heartbeats, idle cleanup,
and login expiration reminders.
See [Integration Verification](docs/testing.md) for full workflows, restart recovery,
file regression tests, sanitizers, and standalone client verification.
See [Concurrency and Performance](docs/performance.md) for reproducible load tests,
measurement definitions, and results from the local test environment.

## Building

Requirements: Linux, a C++17 compiler, CMake 3.20 or later, and Git.
Building the server also requires pkg-config, the MySQL client development library,
and OpenSSL 3.0 or later (`sudo apt install pkg-config libmysqlclient-dev libssl-dev`
on Ubuntu).
The first configuration requires access to GitHub to download pinned versions of
nlohmann/json, cpp-httplib, and Catch2.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The build provides these targets:

| Target | Configuration | Description |
| --- | --- | --- |
| `chatroom_common` | `common/CMakeLists.txt` | Shared protocol, framing, logging, and resource utilities |
| `chatroom_net` | `server/CMakeLists.txt` | Server network library |
| `chatroom_app` | `server/CMakeLists.txt` | Configuration, sessions, request dispatcher, handlers, and worker pool |
| `chatroom_db` | `server/CMakeLists.txt` | MySQL connections, transactions, and chat persistence |
| `chatroom_auth` | `server/CMakeLists.txt` | Password hashing, registration, login, and JWT issuance and verification |
| `chatroom_http` | `server/CMakeLists.txt` | HTTP authentication, file transfers, and request logging |
| `chatroom_server` | `server/CMakeLists.txt` | TCP chat and HTTP authentication server |
| `chatroom_client_net` | `client/CMakeLists.txt` | Blocking TCP client library |
| `chatroom_client_app` | `client/CMakeLists.txt` | HTTP authentication, streamed file transfers, and console commands |
| `chatroom_client` | `client/CMakeLists.txt` | Command-line chat client |
| `chatroom_tests` | `tests/CMakeLists.txt` | Catch2 tests, automatically registered with CTest |
| `chatroom_database_tests` | `tests/CMakeLists.txt` | Optional MySQL and HTTP integration tests (`BUILD_DATABASE_TESTS=ON`) |

To build without tests or the Catch2 dependency:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release -j2
```

To configure only the server or only the client, with tests disabled:

```bash
cmake -S . -B build-server -DBUILD_CLIENT=OFF -DBUILD_TESTING=OFF
cmake --build build-server -j2

cmake -S . -B build-client -DBUILD_SERVER=OFF -DBUILD_TESTING=OFF
cmake --build build-client -j2
```

The server and client run as separate executables and communicate over the
network. Shared code contains the protocol, framing, logging, and small resource utilities. Each component's
dependencies belong in its own `CMakeLists.txt`; shared dependencies belong in
`cmake/Dependencies.cmake`.

Both components use the same pinned cpp-httplib release. The server additionally
uses jwt-cpp and system OpenSSL libraries. The standalone client requires no
MySQL, jwt-cpp, or OpenSSL development files.

## Running the Server

Set up MySQL and import `sql/schema.sql` as described in the
[database guide](docs/database.md), then supply the password in the current shell:

```bash
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_JWT_SECRET="$(openssl rand -hex 32)"
./build/server/chatroom_server
./build/server/chatroom_server --address 0.0.0.0 --port 8081 --workers 4
./build/server/chatroom_server --max-connections 512
./build/server/chatroom_server --idle-timeout 90
cp config/server.example.json config/server.json
./build/server/chatroom_server --config config/server.json
```

The default address is `127.0.0.1:8081`. Port `0` lets the operating system choose
an available port, which appears in the `server_started` JSON log on standard error.
Use `--help` to list options and Ctrl+C to terminate the server. Database passwords
and JWT secrets are read from `CHATROOM_DB_PASSWORD` and `CHATROOM_JWT_SECRET`.
Startup checks database credentials and requires a JWT secret of at least 32 bytes.
HTTP defaults to port 8080; `--http-port` and `--http-workers` configure its listener.
See the
[configuration guide](docs/configuration.md) for the full precedence rules.

The default active-connection limit is 1024. Excess connections are closed; if
accepting runs out of descriptors or socket memory, the Reactor retries after
200 ms while continuing to serve established connections. Choose the connection
limit with room for other file descriptors; it does not raise the OS limit.

Idle TCP connections expire after 90 seconds without a complete incoming frame.
The console sends a heartbeat every 30 seconds and waits up to 60 seconds for its
pong. Both sides have configurable timeouts. Expired logins prompt `/login` again
without closing a responsive connection. See [Connection Keepalive](docs/keepalive.md).

Request admission defaults to 32 unfinished requests per connection and 1024 in
total. Use `--max-requests-per-connection` and `--max-pending-requests`, or the JSON
settings, to change them. Exceeding either limit closes that connection and drops
its waiting requests. Submitted tasks keep their global slots until completion,
even after disconnect. See [Request Capacity](docs/threading.md#request-capacity).

Each request is a four-byte unsigned payload length in network byte order followed
by UTF-8 JSON. A request must include `type`, `request_id`, and `action`; `data` is
an optional object. Authentication, private and group messages, membership queries,
and acknowledgments use the same envelope. Invalid requests receive
structured error responses. JSON nesting is limited to 64 levels, counting the
root object as level 1; oversized frames close the connection. See the
[protocol reference](docs/protocol.md) for the complete format. Plain text sent
with a terminal tool is not a framed request.

Run all tests with CTest, or select the networking tests directly:

```bash
./build/tests/chatroom_tests "[server]"
```

## Running the Client

Start the server, then open another terminal:

```bash
./build/client/chatroom_client
./build/client/chatroom_client 127.0.0.1 8081
```

The default destination is `127.0.0.1:8081`, with HTTP on port 8080. Addresses must
be numeric IPv4 addresses. Add an HTTP port as the third positional argument when
needed: `chatroom_client 127.0.0.1 8081 8080`.

```text
/register alice example-password
/login alice example-password
/send 2 Hello Bob
/send 2,3 Hello everyone
/create_group "Study group" 2,3
/groups
/members 42
/send_group 42 Hello group
/upload 2 "./notes final.pdf"
/download 42 "./received notes.pdf"
/ping
/help
```

Registration prints the new user ID; use actual recipient IDs in `/send`. Login
automatically authenticates TCP without displaying the JWT. Messages and errors
appear as readable text, and the client automatically acknowledges deliveries.
See the [client guide](docs/client.md) for quoted credentials, limits, and JSON
debugging, or the [two-client walkthrough](docs/messaging.md) to try a conversation.

Use `/quit` to cancel pending I/O and exit immediately. End standard input with
Ctrl+D to finish sending and wait for the server's final replies. The client also
exits when the server disconnects, without requiring another line of input.

Requests can also be piped into the client:

```bash
printf '%s\n' '/ping' | ./build/client/chatroom_client
```

To run only the client network tests:

```bash
./build/tests/chatroom_tests "[client]"
```

To run protocol and request-handler tests:

```bash
./build/tests/chatroom_tests "[protocol]"
```
