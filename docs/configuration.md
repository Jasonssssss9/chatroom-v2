# Configuration and Logging

## Starting the Server

The server runs with built-in defaults when no options are supplied. A JSON file
is loaded only when selected with `--config`; it is not discovered automatically.

```bash
./build/server/chatroom_server --help
cp config/server.example.json config/server.json
./build/server/chatroom_server --config config/server.json
./build/server/chatroom_server --config config/server.json --port 9000 --workers 2
```

`config/server.json` is ignored by Git. Keep the tracked example free of local
credentials. Paths are relative to the process's working directory.

Configuration is applied in this order, with later sources overriding earlier ones:

1. Built-in defaults.
2. The JSON file selected by `--config`.
3. Database and JWT environment variables.
4. Command-line overrides for the server and logging.

The position of `--config` among the options does not affect this order. Repeated
options use their last value. Options use separate arguments, such as `--port 9000`.
Use `--help` on its own to print usage without loading configuration.

## JSON Settings

The complete example is [server.example.json](../config/server.example.json).
All sections and fields are optional. Omitted values retain their defaults; unknown
fields are ignored. The root and each supplied section must be JSON objects.

| JSON field | Default | Meaning |
| --- | --- | --- |
| `server.address` | `127.0.0.1` | Numeric IPv4 bind address; `0.0.0.0` listens on all interfaces |
| `server.port` | `8081` | TCP port, from 0 to 65535; 0 requests an available port |
| `server.worker_threads` | `4` | Positive integer number of worker threads |
| `server.max_connections` | `1024` | Positive integer limit on active TCP connections |
| `server.max_requests_per_connection` | `32` | Positive integer limit on unfinished requests per connection |
| `server.max_pending_requests` | `1024` | Positive integer limit on unfinished requests across all connections |
| `database.host` | `127.0.0.1` | MySQL host name or address |
| `database.port` | `3306` | MySQL port, from 1 to 65535 |
| `database.name` | `chatroom` | Database name |
| `database.user` | `chatroom` | Database user |
| `log.level` | `info` | Minimum severity: `debug`, `info`, `warn`, or `error` |

Server and database ports, the worker count, and all capacity limits must be JSON
integers, not strings or floating-point numbers. Addresses, database names, and user names must be
non-empty strings. Invalid input stops startup with an error instead of silently
falling back to a default.

## Command-Line Overrides

| Option | Purpose |
| --- | --- |
| `--config PATH` | Read the given JSON file |
| `--address IPv4` | Override `server.address` |
| `--port NUMBER` | Override `server.port` |
| `--workers NUMBER` | Override `server.worker_threads` |
| `--max-connections NUMBER` | Override `server.max_connections` |
| `--max-requests-per-connection NUMBER` | Override `server.max_requests_per_connection` |
| `--max-pending-requests NUMBER` | Override `server.max_pending_requests` |
| `--log-level LEVEL` | Override `log.level` |
| `--help` | Show usage and exit |

For example, this starts one Reactor and two workers with a dynamically assigned
port. The `server_started` log reports the actual port.

```bash
./build/server/chatroom_server --port 0 --workers 2 --log-level debug
```

The connection limit does not reserve or raise file-descriptor limits. Leave room
for other descriptors when choosing it. See [Networking](networking.md#accept-errors-and-connection-capacity)
for rejection and resource-recovery behavior.

Request limits include submitted tasks and completions waiting for the Reactor.
Exceeding either request limit closes the client connection. Disconnecting drops
waiting requests but does not release a submitted task's slot early. See
[Request Capacity](threading.md#request-capacity) for accounting and overload behavior.

## Database and JWT Environment Variables

| Variable | Overrides or supplies |
| --- | --- |
| `CHATROOM_DB_HOST` | `database.host` |
| `CHATROOM_DB_PORT` | `database.port` |
| `CHATROOM_DB_NAME` | `database.name` |
| `CHATROOM_DB_USER` | `database.user` |
| `CHATROOM_DB_PASSWORD` | Database password; no JSON or CLI equivalent |
| `CHATROOM_JWT_SECRET` | JWT signing secret; no JSON or CLI equivalent |

```bash
export CHATROOM_DB_HOST=127.0.0.1
export CHATROOM_DB_PORT=3306
export CHATROOM_DB_NAME=chatroom
export CHATROOM_DB_USER=chatroom
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
echo
export CHATROOM_DB_PASSWORD
read -r -s -p 'JWT secret: ' CHATROOM_JWT_SECRET
echo
export CHATROOM_JWT_SECRET
./build/server/chatroom_server --config config/server.json
```

The program reads the process environment; it does not load `.env` files. A missing
password or JWT secret currently produces an empty configuration value. They are
optional while the server only handles `ping`. Database access and JWT authentication
are still planned: this step stores their settings without opening MySQL connections
or issuing tokens. Those modules will validate the credentials they require when
they are implemented.

## Log Format

Server logs go to standard error, one JSON object per line. The logger uses a mutex
to keep Reactor and worker records from interleaving. Timestamps use UTC with
millisecond precision.

```json
{"timestamp":"2026-10-16T12:00:00.000Z","level":"info","event":"server_started","context":{"address":"127.0.0.1","port":8081,"worker_threads":4,"max_connections":1024,"max_requests_per_connection":32,"max_pending_requests":1024}}
{"timestamp":"2026-10-16T12:00:01.000Z","level":"info","event":"connection_opened","context":{"connection_id":2,"peer":"127.0.0.1:45000"}}
{"timestamp":"2026-10-16T12:00:01.010Z","level":"warn","event":"request_error","context":{"connection_id":2,"request_id":"r1","action":"missing","code":"unknown_action","error":"Action is not supported"}}
```

| Event | Level | Useful context |
| --- | --- | --- |
| `server_starting` | `info` | Requested bind address, port, worker count, and capacity limits |
| `server_started` | `info` | Actual port, worker count, and capacity limits after successful setup |
| `server_failed` | `error` | Startup or runtime phase and exception details |
| `server_stopped` | `info` | The event loop returned normally |
| `connection_opened` | `info` | Connection ID and peer IP/port |
| `connection_closed` | `info` | Connection ID, peer IP/port, and close reason |
| `connection_error` | `warn` | Connection ID and error details; truncated EOF also includes code `truncated_frame` and peer |
| `connection_rejected` | `warn` | Peer, connection-limit reason, and configured limit |
| `request_rejected` | `warn` | Connection ID, limit reason, current request counts, and configured limits |
| `accept_paused` | `warn` | Resource error number, details, and retry delay |
| `accept_retry` | `debug` | A scheduled attempt to resume accepting connections |
| `request_error` | `warn` or `error` | Connection ID, valid request metadata, error code and details |
| `request_completed` | `debug` | Connection ID, request ID, and action |
| `worker_task_failed` | `error` | An uncaught standalone task exception |

Protocol and application rejections use `warn`; unexpected handler exceptions use
`error`. Invalid request metadata appears as `null`. Correlate the connection ID
with `connection_opened` to find the peer. Connection IDs are unique within one
server process. Successful requests are logged only at `debug` to keep ordinary
traffic quiet. The configured threshold applies to every event, including startup
and connection events.

Logs do not include configuration dumps, database passwords, JWT secrets, tokens,
or request bodies. Handler exceptions should also avoid including credentials in
their messages. A configuration syntax error names the file without echoing its
contents. JSON encoding escapes line breaks in error text, preserving one record
per line.

To save logs, use normal shell redirection:

```bash
./build/server/chatroom_server --config config/server.json 2>server.log
```

The logger is deliberately synchronous and has no background thread, file rotation,
or extra logging dependency. Redirect logs or use the process manager to collect
them. Normal Ctrl+C termination still uses the operating system's default behavior,
so it does not emit `server_stopped`; that event describes a normal `run()` return.

## Tests

```bash
./build/tests/chatroom_tests "[config]"
./build/tests/chatroom_tests "[logger]"
```

Tests cover defaults, JSON settings, CLI precedence, environment overrides, invalid
values, configuration error privacy, log filtering, escaped error details, and
concurrent log output. Networking and dispatcher tests continue to exercise the
same request and connection paths with logging enabled.
