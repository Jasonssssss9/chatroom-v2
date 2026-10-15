# chatroom-server-v2

A chat server and command-line client for Linux, written in C++17.

The planned server uses non-blocking TCP, epoll, and the Reactor pattern for real-time
chat, HTTP and JWT for authentication, and MySQL for users, groups, and messages.
Uploaded files are stored on disk, with their metadata in MySQL.

The project currently contains the directory structure and CMake configuration.
Application features have not been implemented yet.

## Project Layout

```text
cmake/       Third-party dependency configuration
src/         Server source code
client/      Command-line client source code
common/      Protocol code shared by the server and client
tests/       Catch2 tests
config/      Example configuration files
sql/         Database schema scripts
docs/        Design and usage documentation
```

See [Architecture](docs/architecture.md) for the design and project conventions.

## Building

Requirements: Linux, a C++17 compiler, CMake 3.20 or later, and Git.
The first configuration requires access to GitHub to download pinned versions of
nlohmann/json and Catch2.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The source lists are currently empty, so no executables or tests are generated.
As source files are added, list them explicitly in the corresponding
`CMakeLists.txt` to enable these targets:

| Target | Source List | Description |
| --- | --- | --- |
| `chatroom_server` | `SERVER_SOURCES` in `src/CMakeLists.txt` | Server |
| `chatroom_client` | `CLIENT_SOURCES` in `client/CMakeLists.txt` | Command-line client |
| `chatroom_tests` | `TEST_SOURCES` in `tests/CMakeLists.txt` | Catch2 tests, automatically registered with CTest |

To build without tests or the Catch2 dependency:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release -j2
```

Dependencies for HTTP, JWT, MySQL, and password hashing will be added as their
modules are implemented.
