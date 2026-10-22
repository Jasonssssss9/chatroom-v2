# Connection Keepalive

Connection liveness and login validity have separate lifetimes. The console sends
ordinary TCP `ping` requests in the background. The server expires idle connections;
JWT expiration still controls access to business operations.

## Defaults and Configuration

| Setting | Default | Range |
| --- | --- | --- |
| Server `server.idle_timeout_seconds` | 90 seconds | 1 to 86400 seconds |
| Client `--heartbeat-interval` | 30 seconds | 1 to 3600 seconds |
| Client `--heartbeat-timeout` | 60 seconds | 1 to 3600 seconds |

The server accepts JSON configuration and a CLI override:

```json
{"server":{"idle_timeout_seconds":90}}
```

```bash
./build/server/chatroom_server --idle-timeout 90
./build/client/chatroom_client --heartbeat-interval 30 --heartbeat-timeout 60
```

Choose a server idle timeout comfortably longer than the client's heartbeat
interval. These options belong to separate processes, so the client does not
negotiate them with the server. The defaults leave room for network and scheduling
delays. JSON and CLI values must be integers in the documented ranges.

## Server Idle Cleanup

`TcpConnection` records a `steady_clock` timestamp when accepted and whenever it
extracts a complete frame. An incomplete header or payload does not refresh it.
Outgoing traffic does not refresh it either: broadcasting to a silent client
does not prove that the client is still responsive. JSON validation remains on
workers, so complete frames count as activity before application validation.

The Reactor scans connections at most once per second. The scan shares
`epoll_wait()`'s timeout with the existing accept retry deadline; there is no new
server thread, timer queue, or callback framework. With the default settings,
cleanup can occur up to about one scan interval after the idle deadline, plus
event-loop scheduling delay.

An expired connection closes with reason `idle_timeout` in the structured
`connection_closed` log. The existing disconnect callback removes its session
and waiting requests. Submitted tasks may still finish, but cannot access the
closed socket. Persisted, unacknowledged deliveries remain available after login
on a new connection. Idle cleanup also applies before authentication and while
a worker is busy; an active task does not indefinitely retain a silent socket.

## Client Heartbeats

The console uses one receiver thread and one small timer thread in addition to
the input thread. The timer continues during blocking HTTP registration, login,
uploads, and downloads. All TCP writes share the existing send mutex, including
terminal requests, ACKs, heartbeats, and the stdin EOF half-close.

After the interval, the timer sends:

```json
{"type":"request","request_id":"heartbeat-1","action":"ping"}
```

Only one automatic probe may be outstanding. A successful `ping` response with
the same request ID and `data.pong: true` must arrive before its deadline.
Unrelated responses, notifications, and partial frames do not confirm that probe.
After a matching pong, the interval starts again. Heartbeat IDs use their own
`heartbeat-` prefix; raw clients should avoid that prefix for manual requests.

Normal output suppresses automatic pongs. `/ping` still displays `Pong.`;
`--json` displays automatic responses as well. Heartbeats use the existing
request queue and per-connection ordering, so a server unable to handle requests
within the configured timeout is considered unresponsive.

On timeout, the console cancels TCP I/O and exits with a message:

```text
Client error: Heartbeat timed out. Reconnect and use /login to continue
```

TCP sends have a five-second socket timeout so a stalled sender cannot hold the
send mutex indefinitely. Receiver termination also cancels in-flight HTTP calls.
Server EOF prints a reconnection hint even when stdin is idle. No automatic
reconnection or resend is performed.

`/quit`, EOF, and receiver termination wake and stop the timer. EOF retains the
existing half-close behavior: final replies are displayed, with no further
heartbeats or ACKs. All console threads join before their dependencies are destroyed.

## Login Expiration

Successful TCP authentication now includes the token's expiration as Unix seconds:

```json
{"type":"response","request_id":"auth-1","action":"authenticate","ok":true,"data":{"user_id":"1","expires_at":2000000000}}
```

The timestamp comes from the verified JWT. The timer wakes at that deadline,
clears the console's HTTP file token, and prints this once:

```text
Login expired. Use /login to authenticate again.
```

The connection remains open and heartbeats continue. Use `/login` for the same
user to refresh the session; successful authentication replaces the previous
expiry deadline. A failed login or rejected account switch preserves the existing
session and deadline. Reconnect to change users.

This local reminder depends on the client's clock. Server-side expiration checks
remain authoritative. Business errors also include a `/login` hint, and HTTP file
authentication failures point to the same command. Older peers that omit
`expires_at` still work but cannot provide the proactive reminder.

## Verification

Tests cover idle cleanup, incomplete input, active peers, cleanup during worker
execution, matching and unrelated pongs, partial replies, cancellation without
stdin activity, heartbeats during HTTP uploads, token expiration, and refresh.
Networking tests use short timeouts; console tests use one-second intervals.

```bash
./build/tests/chatroom_tests "[heartbeat]"
```
