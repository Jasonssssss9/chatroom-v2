# Architecture

The server runs HTTP and TCP services in one process. The command-line client is
a separate executable. They share protocol utilities and communicate over the
network; neither links to the other's implementation.

## Responsibilities

| Component | Responsibility |
| --- | --- |
| HTTP service | Registration, login, JWT issuance, and streamed file transfers |
| TCP Reactor | Socket ownership, framing, buffered I/O, online sessions, and idle cleanup |
| TCP worker pool | JSON validation, application requests, SQL, and replay queries |
| HTTP worker pool | Password hashing, SQL, and file I/O |
| MySQL | Users, groups, messages, delivery acknowledgments, and file metadata |
| File storage | Uploaded bytes, addressed by server-generated storage keys |
| Console client | Commands, HTTP login, TCP authentication, display, ACKs, and heartbeats |

The TCP path uses one non-blocking epoll Reactor and four workers by default.
Workers return results through an eventfd-backed queue. The Reactor applies each
result before submitting the next request from that connection. HTTP has a
separate fixed pool of four workers and its own listener thread.

A message and its recipient deliveries commit before the sender receives success
or recipients receive notifications. Unacknowledged deliveries replay after
authentication. File bytes travel over HTTP; their TCP notifications use the
same delivery pipeline. See [Threading](threading.md), [Messaging](messaging.md),
and [Files](files.md) for lifecycle details.

## Project Layout

| Path | Contents |
| --- | --- |
| `server/src/net/` | Linux sockets, epoll, connections, and Reactor |
| `server/src/db/` | MySQL connections, transactions, and chat operations |
| `server/src/auth/` | Password hashing, user authentication, and JWTs |
| `server/src/http/` | HTTP endpoints and request logging |
| `server/src/` | Configuration, sessions, worker pool, dispatch, and entry point |
| `client/src/` | Console commands, HTTP clients, and TCP client |
| `common/` | Framing, JSON protocol, logging, descriptor ownership, and pending files |
| `cmake/` | Shared dependency configuration |
| `config/` | Tracked example and optional local configuration |
| `sql/` | Schema and explicit migrations |
| `tests/` | Catch2 cases, process integration, and performance harness |
| `docs/` | Usage, protocol, design, and verification references |
| `build/` | Generated CMake files, dependencies, binaries, and test metadata |
| `data/files/` | Default runtime file storage; excluded from Git |

## Build Options

The project requires Linux, C++17, and CMake 3.20 or later. The server additionally
requires pkg-config, MySQL client development files, and OpenSSL 3.0 or later.
The standalone client requires none of those server dependencies.

| CMake option | Default | Effect |
| --- | --- | --- |
| `BUILD_SERVER` | `ON` | Build the server and its libraries |
| `BUILD_CLIENT` | `ON` | Build the console client and its libraries |
| `BUILD_TESTING` | `ON` | Build Catch2 tests and register them with CTest |
| `BUILD_DATABASE_TESTS` | `OFF` | Add MySQL tests; requires server and testing enabled |

For a server-only Release build:

```bash
cmake -S . -B build-server -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLIENT=OFF -DBUILD_TESTING=OFF -DBUILD_DATABASE_TESTS=OFF
cmake --build build-server -j2
```

For a client-only build, see [Standalone Client](testing.md#standalone-client).

| Target | Purpose |
| --- | --- |
| `chatroom_common` | Shared utilities and wire protocol |
| `chatroom_net` | Server network layer |
| `chatroom_db` | MySQL access and persistence |
| `chatroom_auth` | Passwords, user authentication, and JWTs |
| `chatroom_http` | HTTP authentication and files |
| `chatroom_app` | Configuration, sessions, dispatch, and worker pool |
| `chatroom_server` | Server executable |
| `chatroom_client_net` | Blocking TCP client |
| `chatroom_client_app` | Console and HTTP clients |
| `chatroom_client` | Client executable |
| `chatroom_tests` | Ordinary Catch2 suite |
| `chatroom_database_tests` | Optional MySQL integration suite |

CMake fetches pinned versions of nlohmann/json 3.11.3, cpp-httplib 0.62.0,
jwt-cpp 0.7.2, and Catch2 3.8.1. jwt-cpp is server-only; Catch2 is needed only
for tests. Component dependencies belong in each component's `CMakeLists.txt`;
shared dependency setup lives in `cmake/Dependencies.cmake`.

## Conventions and Scope

Use straightforward C++17, RAII, and concrete classes with clear ownership.
Add abstractions when a feature needs them. Keep project text, comments, and
runtime messages in idiomatic English. Commit messages follow
`feat(scope): description` or `fix(scope): description`.

The application uses IPv4 TCP and HTTP without built-in TLS. It has one online
connection per user, fixed worker pools, and synchronous SQL on workers. Group
membership is set at creation; file retention, automatic reconnection, and token
revocation are not implemented. Measured capacity and database bottlenecks are
documented in [Performance](performance.md).
