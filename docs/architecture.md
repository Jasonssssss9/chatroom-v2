# Architecture

## Project Conventions

- Use C++17. Keep code and CMake straightforward, and add abstractions only when
  a feature needs them.
- Use idiomatic English for all project text, including documentation, comments,
  and runtime messages.
- Use conventional commit messages such as `feat(database): description` or
  `fix(protocol): description`.
- Run the HTTP API and TCP chat service in a single process.
- Keep the server and command-line client in the same repository and build them
  as separate executables.
- Keep their implementations in `server/src/` and `client/src/`. Shared code in
  `common/` contains the protocol, framing, logging, and small resource utilities;
  neither component links to the other's implementation.
- Configure component-specific dependencies in each component's `CMakeLists.txt`.
  Use `BUILD_SERVER` and `BUILD_CLIENT` to select which components to build.

## Responsibilities

- HTTP: registration, login, JWT issuance, and file uploads and downloads.
- TCP: connection authentication, direct messages, messages to multiple users,
  group chat, notifications, acknowledgments, and heartbeats.
- Reactor: exclusive ownership of sockets, connection state, and I/O buffers.
- Worker pool: business logic and database operations, with results returned to
  the Reactor through a queue.
- MySQL: users, groups, messages, delivery status, and file metadata.
- Disk storage: uploaded file contents, referenced by storage keys in MySQL.

Each TCP message consists of a four-byte length in network byte order followed by
a JSON payload. Direct messages are persisted before the server reports success
to the sender and notifies online recipients. Receivers acknowledge deliveries;
unacknowledged messages replay after authentication. The console client suppresses
recent duplicates and acknowledges repeated deliveries.

## Current Status

The server network layer implements non-blocking IPv4 TCP, an epoll ET Reactor,
message framing, buffered writes, and cross-thread sends through an eventfd-backed
queue. A configurable pool (four workers by default) parses and handles requests,
with per-connection ordering and completions returned to the Reactor. The executable
implements `ping`, `authenticate`, `send_message`, `ack_message`, and
correlated protocol errors. Catch2 covers framing, request validation, handlers,
connection I/O, and TCP integration. The command-line client uses a blocking TCP
connection with separate input and receive threads. Its library and tests can be
built without the server. See [Networking](networking.md), [JSON Protocol](protocol.md),
and [Worker Pool and Request Handoff](threading.md).

The console provides registration, login, and messaging commands. Its `AuthClient`
uses the same cpp-httplib dependency as the server, without linking server code.
HTTP login passes the JWT directly to TCP authentication. Normal output formats
messages and errors; `--json` preserves raw TCP display. See [Command-Line Client](client.md).

JSON configuration and command-line options control the bind address, port, worker
count, connection and request limits, and log level. The dispatcher limits
unfinished requests, retaining submitted tasks in the global count after disconnect.
The Reactor pauses and retries accepting
when descriptors or socket memory are exhausted, while serving existing clients.
Database settings and JWT secrets can be supplied through
environment variables. Startup, connections, and request failures use a shared
JSON logger. See [Configuration and Logging](configuration.md).

MySQL persistence is implemented through the C API, with prepared statements,
RAII connections and transactions, and concrete operations for users, groups,
messages, acknowledgments, and file metadata. The executable checks connectivity
before starting workers. Database integration tests use a separate disposable
database. See [MySQL Persistence](database.md).

HTTP registration and login are implemented with cpp-httplib, OpenSSL scrypt,
and jwt-cpp HS256 token issuance. HTTP uses a separate fixed worker pool; each
worker reuses its own database connection. Both listeners run in the same process.
See [HTTP Authentication](authentication.md).

TCP verifies JWT signatures and claims, checks that the user exists, and returns
the identity to the Reactor. `SessionManager` keeps connection-to-user and
user-to-connection maps on the Reactor thread. A new authenticated connection
replaces the previous one; disconnect removes its binding. Workers receive
session copies, and expired sessions cannot authorize business requests or online
routing.

`send_message` validates recipients and stores one message with its delivery rows
in a transaction. A worker returns a serialized notification and recipient user
IDs. The Reactor looks up their current connections and queues online delivery,
even if the sender disconnected after the task started. Exact sender retries
reuse the stored ID and may notify recipients again. `ack_message` confirms only
the session user's delivery. After binding a session, the Reactor schedules
bounded replay on workers and waits for a batch's ACKs before fetching more.
Group creation, member queries, and group messages are implemented. Group sends
check membership and retain their original recipient snapshot across retries.
Group and private messages share ACKs, replay, and online routing.
HTTP uploads and downloads stream file bytes on HTTP workers. File metadata,
permissions, and the file notification's message and deliveries commit together;
the Reactor routes the resulting `file_available` notice. Files use the same ACK
and replay pipeline as text. See [Messaging](messaging.md), [Group Chat](groups.md),
and [File Transfers](files.md).
