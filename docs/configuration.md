# Configuration and Logging

The server starts with built-in defaults. It reads a JSON file only when selected
with `--config`; it does not discover configuration or load `.env` files.

## Precedence

Later sources override earlier ones:

1. Built-in defaults.
2. The JSON file selected by `--config`.
3. Database and JWT environment variables.
4. Command-line overrides.

The position of `--config` does not change this order. Repeated options use their
last value. Options take separate arguments, such as `--port 9000`. Use
`--help` on its own to print usage without loading configuration.

Copy [server.example.json](../config/server.example.json) to
`config/server.json` for local changes; the latter is ignored by Git.
Paths are relative to the server's working directory.

## JSON Settings

All fields are optional. The root and supplied sections must be objects; unknown
fields are ignored. Numeric settings must be integers. Invalid values stop
startup instead of falling back to defaults.

| Field | Default | Meaning |
| --- | --- | --- |
| `server.address` | `127.0.0.1` | Numeric IPv4; `0.0.0.0` listens on all interfaces |
| `server.port` | `8081` | TCP port, 0–65535; 0 selects an available port |
| `server.worker_threads` | `4` | Positive TCP worker count |
| `server.max_connections` | `1024` | Positive active TCP connection limit |
| `server.max_requests_per_connection` | `32` | Positive unfinished-request limit per connection |
| `server.max_pending_requests` | `1024` | Positive unfinished-request limit across connections |
| `server.idle_timeout_seconds` | `90` | TCP idle timeout, 1–86400 seconds |
| `http.port` | `8080` | HTTP port, 0–65535; shares `server.address` |
| `http.worker_threads` | `4` | Positive HTTP worker count |
| `jwt.token_lifetime_seconds` | `3600` | Token lifetime, 1–86400 seconds |
| `files.directory` | `data/files` | Nonempty storage directory; created on first upload |
| `files.max_size_bytes` | `10485760` | Upload limit, 1 byte–1 GiB; empty files are allowed |
| `database.host` | `127.0.0.1` | Nonempty MySQL host |
| `database.port` | `3306` | MySQL port, 1–65535 |
| `database.name` | `chatroom` | Nonempty database name |
| `database.user` | `chatroom` | Nonempty database user |
| `log.level` | `info` | `debug`, `info`, `warn`, or `error` |

## Command-Line Options

| Option | Overrides |
| --- | --- |
| `--config PATH` | Select the JSON configuration file |
| `--address IPv4` | `server.address` |
| `--port NUMBER` | `server.port` |
| `--workers NUMBER` | `server.worker_threads` |
| `--http-port NUMBER` | `http.port` |
| `--http-workers NUMBER` | `http.worker_threads` |
| `--max-connections NUMBER` | `server.max_connections` |
| `--idle-timeout NUMBER` | `server.idle_timeout_seconds` |
| `--max-requests-per-connection NUMBER` | `server.max_requests_per_connection` |
| `--max-pending-requests NUMBER` | `server.max_pending_requests` |
| `--log-level LEVEL` | `log.level` |
| `--help` | Print usage and exit |

For example, after setting the required environment variables:

```bash
./build/server/chatroom_server --config config/server.json --port 0 --workers 2
```

The `server_started` log reports the actual TCP `port` and `http_port`.
Connection limits do not raise the operating system's file-descriptor limit.
Request limits count waiting, submitted, running, and completed-but-unapplied
tasks. Exceeding either request limit closes the offending connection. See
[Networking](networking.md#accept-errors-and-connection-capacity) and
[Request Capacity](threading.md#request-capacity).

## Environment Variables

| Variable | Supplies or overrides |
| --- | --- |
| `CHATROOM_DB_HOST` | `database.host` |
| `CHATROOM_DB_PORT` | `database.port` |
| `CHATROOM_DB_NAME` | `database.name` |
| `CHATROOM_DB_USER` | `database.user` |
| `CHATROOM_DB_PASSWORD` | Database password; no JSON or CLI equivalent |
| `CHATROOM_JWT_SECRET` | JWT signing secret; no JSON or CLI equivalent |

The [README](../README.md#4-start-the-server) shows how to supply these values
without echoing the password. The JWT secret must contain at least 32 bytes.
Keep it stable across restarts if existing tokens should remain valid.

Startup checks database connectivity and validates the JWT configuration before
serving requests. The server does not create databases, import tables, or apply
migrations. See [Database](database.md).

## Logs

Logs go to standard error as one JSON object per line. Timestamps use UTC with
millisecond precision. A mutex keeps records from different threads intact.

```json
{"timestamp":"2026-10-21T12:00:01.000Z","level":"info","event":"connection_opened","context":{"connection_id":3,"peer":"127.0.0.1:45000"}}
```

| Events | Useful context |
| --- | --- |
| `server_starting`, `server_started` | Bind address, requested or actual ports, worker counts, and limits |
| `database_connected` | Database host, port, and name |
| `server_failed`, `server_stopped` | Failure phase and details, or completed shutdown |
| `connection_opened`, `connection_closed` | Connection ID, peer, and close reason |
| `connection_error`, `connection_rejected` | I/O errors, truncated frames, or connection-limit rejection |
| `accept_paused`, `accept_retry` | Resource error and retry delay |
| `session_authenticated`, `session_replaced` | User and connection IDs |
| `request_rejected` | Capacity reason, counts, and limits |
| `request_error`, `request_completed` | Request ID, action, and error details when present |
| `message_replay_failed` | Replay query or serialization failure |
| `http_request` | Client IP, method, URI path, status, and structured error |
| `http_database_error`, `http_request_failed` | MySQL error code or unexpected HTTP failure |
| `worker_task_failed` | Unexpected standalone task exception |

Successful TCP requests and accept retries use `debug`. Protocol rejections use
`warn`; unexpected failures use `error`. Other lifecycle events normally use
`info`. The threshold applies to every event.

Connection IDs are unique within a server process. Correlate request errors with
`connection_opened` to find the peer. Invalid request metadata appears as
`null`. Idle cleanup uses close reason `idle_timeout`.

Logs omit passwords, JWT secrets, tokens, request bodies, Authorization headers,
and HTTP query strings. MySQL errors report operation names and codes without
SQL values. JSON encoding escapes embedded line breaks.

The logger writes synchronously and provides no rotation or background thread.
Use shell redirection or a process manager:

```bash
./build/server/chatroom_server 2>server.log
```

SIGINT and SIGTERM trigger normal shutdown. `server_stopped` is emitted after
HTTP and TCP workers have joined.
