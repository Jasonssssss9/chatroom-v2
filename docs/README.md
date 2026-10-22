# Documentation

Start with the [project README](../README.md) to build the application and try the main features with two clients. Run commands from the repository root unless a guide says otherwise.

## Usage and Verification

| Guide | Contents |
| --- | --- |
| [Client](client.md) | Commands, quoting, output modes, and connection lifecycle |
| [Configuration](configuration.md) | JSON settings, environment variables, CLI options, and logs |
| [Testing](testing.md) | Catch2, isolated MySQL tests, process integration, and sanitizers |
| [Performance](performance.md) | Reproducible workloads, measured results, and limitations |

## Features and Protocols

| Guide | Contents |
| --- | --- |
| [TCP protocol](protocol.md) | Framing, request and response formats, actions, and errors |
| [Authentication](authentication.md) | HTTP registration and login, JWTs, and TCP sessions |
| [Messaging](messaging.md) | Private and multi-recipient sends, retries, ACKs, and offline replay |
| [Groups](groups.md) | Creation, discovery, membership queries, and broadcasts |
| [Files](files.md) | HTTP upload and download, access control, storage, and notifications |
| [Keepalive](keepalive.md) | Heartbeats, idle cleanup, and login expiration |

## Implementation

| Guide | Contents |
| --- | --- |
| [Architecture](architecture.md) | Module boundaries, project layout, dependencies, and build options |
| [Networking](networking.md) | Socket ownership, epoll, framing, buffered I/O, and shutdown |
| [Threading](threading.md) | Worker handoff, request ordering, capacity, and completion handling |
| [Database](database.md) | Schema, migrations, connection ownership, and transaction behavior |
