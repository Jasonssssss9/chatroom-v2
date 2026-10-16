# chatroom-server-v2

A chat server and command-line client for Linux, written in C++17.

The planned server uses non-blocking TCP, epoll, and the Reactor pattern for real-time
chat, HTTP and JWT for authentication, and MySQL for users, groups, and messages.
Uploaded files are stored on disk, with their metadata in MySQL.

The server and client network layers are implemented. The server currently echoes
framed JSON messages, and the command-line client sends one JSON value per line.
Authentication, chat features, and database access are still planned.

## Project Layout

```text
server/
    CMakeLists.txt   Server build configuration
    src/            Server source code
client/
    CMakeLists.txt   Client build configuration
    src/            Command-line client source code
common/             Message framing and file descriptor ownership
tests/              Catch2 tests
cmake/              Shared third-party dependency configuration
config/             Example configuration files
sql/                Database schema scripts
docs/               Design and usage documentation
```

See [Architecture](docs/architecture.md) for the design and project conventions,
and [Networking](docs/networking.md) for the current network API and behavior.

## Building

Requirements: Linux, a C++17 compiler, CMake 3.20 or later, and Git.
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
| `chatroom_common` | `common/CMakeLists.txt` | Shared framing library |
| `chatroom_net` | `server/CMakeLists.txt` | Server network library |
| `chatroom_server` | `server/CMakeLists.txt` | JSON echo server |
| `chatroom_client_net` | `client/CMakeLists.txt` | Blocking TCP client library |
| `chatroom_client` | `client/CMakeLists.txt` | Command-line JSON client |
| `chatroom_tests` | `tests/CMakeLists.txt` | Catch2 tests, automatically registered with CTest |

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
network. Shared code contains framing and small resource utilities. Each component's
dependencies belong in its own `CMakeLists.txt`; shared dependencies belong in
`cmake/Dependencies.cmake`.

Dependencies for HTTP, JWT, MySQL, and password hashing will be added as their
modules are implemented.

## Running the Server

```bash
./build/server/chatroom_server
./build/server/chatroom_server 8081 0.0.0.0
```

The default address is `127.0.0.1:8081`. Port `0` lets the operating system choose
an available port, which is printed at startup. Use Ctrl+C to terminate the demo.

Each request is a four-byte unsigned payload length in network byte order followed
by UTF-8 JSON. The server returns the parsed JSON in the same framing format.
Malformed JSON receives `{"error":"invalid_json"}`; oversized frames close the
connection. Plain text sent with a terminal tool is not a framed request.

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
Enter one complete JSON value per line, for example `{"text":"hello"}`. Replies
appear with a `<` prefix. Invalid JSON is rejected locally.

Use `/quit` to cancel pending I/O and exit immediately. End standard input with
Ctrl+D to finish sending and wait for the server's final replies. The client also
exits when the server disconnects, without requiring another line of input.

Input can be piped into the echo demo:

```bash
printf '%s\n' '{"text":"hello"}' '{"text":"goodbye"}' | ./build/client/chatroom_client
```

To run only the client network tests:

```bash
./build/tests/chatroom_tests "[client]"
```
