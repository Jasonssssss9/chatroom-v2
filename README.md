# chatroom-server-v2

A chat server and command-line client for Linux, written in C++17.

The server uses non-blocking TCP, epoll, and the Reactor pattern for real-time
chat, HTTP and JWT for authentication, and MySQL for users, groups, and messages.
File storage is planned on disk, with metadata in MySQL.

The server and client network layers are implemented. The server accepts JSON
requests, supports `ping`, `authenticate`, and `send_message`, and returns structured errors. The command-line client
sends one JSON request per line.
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
authentication triggers bounded replay of unacknowledged messages. Group chat
and file transfers are still planned.

## Project Layout

```text
server/
    CMakeLists.txt   Server build configuration
    src/            Server source code
client/
    CMakeLists.txt   Client build configuration
    src/            Command-line client source code
common/             JSON protocol, message framing, logging, and file descriptor ownership
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

## Building

Requirements: Linux, a C++17 compiler, CMake 3.20 or later, and Git.
Building the server also requires pkg-config, the MySQL client development library,
and OpenSSL 3.0 or later (`sudo apt install pkg-config libmysqlclient-dev libssl-dev`
on Ubuntu).
The first configuration requires access to GitHub to download pinned versions of
nlohmann/json and Catch2.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The build provides these targets:

| Target | Configuration | Description |
| --- | --- | --- |
| `chatroom_common` | `common/CMakeLists.txt` | Shared framing, JSON protocol, and logging library |
| `chatroom_net` | `server/CMakeLists.txt` | Server network library |
| `chatroom_app` | `server/CMakeLists.txt` | Configuration, sessions, request dispatcher, handlers, and worker pool |
| `chatroom_db` | `server/CMakeLists.txt` | MySQL connections, transactions, and chat persistence |
| `chatroom_auth` | `server/CMakeLists.txt` | Password hashing, registration, login, and JWT issuance and verification |
| `chatroom_http` | `server/CMakeLists.txt` | HTTP authentication listener and request logging |
| `chatroom_server` | `server/CMakeLists.txt` | TCP chat and HTTP authentication server |
| `chatroom_client_net` | `client/CMakeLists.txt` | Blocking TCP client library |
| `chatroom_client` | `client/CMakeLists.txt` | Command-line JSON client |
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

The server additionally downloads pinned cpp-httplib and jwt-cpp releases and
links system OpenSSL libraries. The standalone client needs neither dependency.

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

Request admission defaults to 32 unfinished requests per connection and 1024 in
total. Use `--max-requests-per-connection` and `--max-pending-requests`, or the JSON
settings, to change them. Exceeding either limit closes that connection and drops
its waiting requests. Submitted tasks keep their global slots until completion,
even after disconnect. See [Request Capacity](docs/threading.md#request-capacity).

Each request is a four-byte unsigned payload length in network byte order followed
by UTF-8 JSON. A request must include `type`, `request_id`, and `action`; `data` is
an optional object. The server supports `ping`, `authenticate`, and `send_message`. Invalid requests receive
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

The default destination is `127.0.0.1:8081`. Addresses must be numeric IPv4 addresses.
Enter one complete JSON request per line:

```json
{"type":"request","request_id":"ping-1","action":"ping"}
```

Replies appear with a `<` prefix and include the matching request ID. Invalid JSON
syntax is rejected locally; envelope and action validation happen on the server.
Input is limited to 64 KiB per line, excluding LF or CRLF. An oversized line is
reported as soon as it exceeds the limit and discarded through its next newline;
later lines can still be sent.

After obtaining a JWT from HTTP login, bind this TCP connection to the user:

```json
{"type":"request","request_id":"auth-1","action":"authenticate","data":{"token":"<JWT>"}}
```

The response contains the authenticated `user_id`. A successful authentication on
another connection closes the user's previous connection. See the
[session rules](docs/authentication.md#tcp-connection-authentication) for expiry and refresh behavior.

Once authenticated, send a message to another user's ID:

```json
{"type":"request","request_id":"send-1","action":"send_message","data":{"client_message_id":"alice-1","recipients":["2"],"text":"Hello Bob"}}
```

Replace `"2"` with the recipient's actual user ID. The sender receives a message
ID after persistence; an online recipient receives a `message` notification.
Add more IDs to `recipients` for a multi-recipient send. See the
[two-client walkthrough](docs/messaging.md) for the complete flow.

Use `/quit` to cancel pending I/O and exit immediately. End standard input with
Ctrl+D to finish sending and wait for the server's final replies. The client also
exits when the server disconnects, without requiring another line of input.

Requests can also be piped into the client:

```bash
printf '%s\n' '{"type":"request","request_id":"ping-1","action":"ping"}' | ./build/client/chatroom_client
```

To run only the client network tests:

```bash
./build/tests/chatroom_tests "[client]"
```

To run protocol and request-handler tests:

```bash
./build/tests/chatroom_tests "[protocol]"
```
