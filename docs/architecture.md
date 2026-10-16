# Architecture

## Project Conventions

- Use C++17. Keep code and CMake straightforward, and add abstractions only when
  a feature needs them.
- Use idiomatic English for all project text, including documentation, comments,
  and runtime messages.
- Use the commit message format `feat(): description`.
- Run the HTTP API and TCP chat service in a single process.
- Keep the server and command-line client in the same repository and build them
  as separate executables.
- Keep their implementations in `server/src/` and `client/src/`. Shared code in
  `common/` contains framing and small resource utilities; neither component links to the
  other's implementation.
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
a JSON payload. The planned delivery flow persists messages before the server
reports success to the sender. Receiving clients will acknowledge messages and
discard duplicates using message IDs. Unacknowledged messages can be delivered again.

## Current Status

The server network layer implements non-blocking IPv4 TCP, an epoll ET Reactor,
message framing, buffered writes, and cross-thread sends through an eventfd-backed
queue. The executable currently provides JSON echo behavior. Catch2 covers framing,
connection I/O, and TCP integration. See [Networking](networking.md).

HTTP, JWT, MySQL, password hashing, and the command-line client have not been
implemented yet. Their dependencies will be added as needed.
