# Connection Keepalive

Connection liveness and login validity have separate lifetimes. The console sends
TCP pings in the background. The server cleans up idle connections, while JWT
expiration controls access to business operations.

## Settings

| Setting | Default | Range |
| --- | --- | --- |
| Server `server.idle_timeout_seconds` or `--idle-timeout` | 90 seconds | 1–86400 |
| Client `--heartbeat-interval` | 30 seconds | 1–3600 |
| Client `--heartbeat-timeout` | 60 seconds | 1–3600 |

Keep the server idle timeout comfortably longer than the heartbeat interval.
The settings belong to separate processes and are not negotiated. See
[Configuration](configuration.md) and [Client](client.md).

## Server Idle Cleanup

A connection records monotonic time when accepted and whenever it extracts a
complete frame. Partial headers, partial payloads, and outgoing traffic do not
refresh the deadline. Complete frames count before JSON validation on workers.

The Reactor scans at most once per second, sharing its epoll timeout with the
accept-retry deadline. No additional server thread is needed. Cleanup can occur
about one scan interval after the deadline, plus event-loop scheduling delay.

Idle connections close with reason `idle_timeout`. Session bindings and waiting
requests are removed. Submitted work can still finish without accessing the
closed socket; persisted unacknowledged deliveries remain available for the next
login. Cleanup applies before authentication and while a task is running.

## Client Heartbeats

The timer thread runs during blocking registration, login, and file transfers.
It sends an ordinary request:

```json
{"type":"request","request_id":"heartbeat-1","action":"ping"}
```

Only one automatic probe is outstanding. A successful response must match its
request ID and contain `data.pong: true` before the deadline. Unrelated replies,
notifications, and partial frames do not confirm it. After a matching pong,
the interval starts again. Raw users should avoid the reserved `heartbeat-`
request-ID prefix.

Automatic pongs are hidden in normal output and visible with `--json`.
Manual `/ping` prints `Pong.`. Heartbeats share the ordinary request queue,
so a server unable to process them within the timeout is considered unresponsive.

On timeout the client cancels I/O and exits:

```text
Client error: Heartbeat timed out. Reconnect and use /login to continue
```

Terminal requests, ACKs, probes, and stdin half-close share a send mutex.
TCP writes have a five-second socket timeout. Receiver termination also cancels
in-flight HTTP calls. A server disconnect wakes the console even if stdin is idle.

`/quit`, EOF, and receiver termination stop the timer. EOF waits for final
replies with no further ACKs or heartbeats. Threads join before their dependencies
are destroyed. The client does not reconnect or resend automatically.

## Login Expiration

Authentication returns `user_id` and `expires_at`, the verified token's Unix
expiration time. The timer clears the console's HTTP file token at that deadline
and prints once:

```text
Login expired. Use /login to authenticate again.
```

The connection stays open and heartbeats continue. Log in again as the same user
to refresh the session and deadline. Failed login or a rejected account switch
preserves the existing session; reconnect to change users.

This reminder uses the client's clock. Server-side checks remain authoritative.
Business and file-authentication errors also point to `/login`. A peer omitting
`expires_at` cannot provide the proactive reminder. See
[Authentication](authentication.md#session-rules).

## Verification

Catch2 covers idle cleanup, partial input, active peers, cleanup during work,
matching and unrelated pongs, cancellation, heartbeat during uploads, expiry,
and refresh:

```bash
./build/tests/chatroom_tests '[heartbeat]'
```
