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
  `common/` is limited to protocol utilities; neither component links to the
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
a JSON payload. Messages are persisted before the server reports success to the
sender. Receiving clients acknowledge messages and discard duplicates using
message IDs. Unacknowledged messages can be delivered again.

## Current Status

The project currently contains the directory structure and CMake configuration,
with dependencies for JSON, threading, and Catch2. HTTP, JWT, MySQL, and password
hashing dependencies will be added as their modules are implemented.
The source lists are empty; server, client, and test implementations have not been
added yet.
