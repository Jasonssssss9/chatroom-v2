# Authentication

HTTP handles registration and login; TCP uses the resulting JWT to bind a
connection to a user. Both listeners run in the same C++ process. Defaults are
HTTP `127.0.0.1:8080` and TCP `127.0.0.1:8081`.

## HTTP API

Both endpoints accept POST with `Content-Type: application/json` and a UTF-8
object containing string `username` and `password` fields. A charset parameter
is accepted; unknown fields are ignored. Bodies are limited to 4096 bytes and
64 nested objects or arrays, counting the root as level 1.

Usernames are case-sensitive and contain 1–64 UTF-8 bytes. Passwords contain
8–128 UTF-8 bytes. Values are used as supplied, including spaces and Unicode.

With curl installed, register a user:

```bash
curl -i http://127.0.0.1:8080/api/users \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"example-password"}'
```

HTTP 201:

```json
{"id":"1","username":"alice"}
```

Log in:

```bash
curl -i http://127.0.0.1:8080/api/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"example-password"}'
```

HTTP 200:

```json
{"access_token":"<JWT>","token_type":"Bearer","expires_in":3600}
```

Responses use JSON and `Cache-Control: no-store`. HTTP does not use the TCP
request envelope. Errors have this shape:

```json
{"error":{"code":"invalid_credentials","message":"Invalid username or password"}}
```

| Status | Code | Meaning |
| --- | --- | --- |
| 400 | `invalid_json` | Malformed JSON or invalid UTF-8 |
| 400 | `invalid_request` | Invalid fields, lengths, root type, or nesting |
| 401 | `invalid_credentials` | Unknown username or incorrect password |
| 404 | `not_found` | Unknown endpoint |
| 405 | `method_not_allowed` | Known authentication endpoint requires POST; includes `Allow: POST` |
| 409 | `username_taken` | Username already exists |
| 413 | `payload_too_large` | Body exceeds 4096 bytes |
| 415 | `unsupported_media_type` | Wrong Content-Type |
| 500 | `internal_error` | Database, hashing, or unexpected failure |

The database's unique constraint handles concurrent registrations. Unknown users
and incorrect passwords receive the same 401 body. Unknown-user attempts still
verify against a dummy hash.

## Passwords and JWTs

OpenSSL scrypt uses `N=32768`, `r=8`, `p=3`, a random 16-byte salt, and a
32-byte derived key. Verification uses constant-time comparison. MySQL stores
`scrypt$32768$8$3$<salt-hex>$<key-hex>`; raw passwords are never stored.

jwt-cpp signs HS256 tokens with `typ=JWT`, `iss=chatroom-v2`, the decimal user
ID in string `sub`, and integer `iat` and `exp` claims. Passwords and hashes
are not claims. The configured lifetime defaults to 3600 seconds.

Supply `CHATROOM_JWT_SECRET` through the process environment, with at least
32 bytes. Keep it stable across restarts to preserve existing tokens. There are
no refresh tokens or revocation lists; logging in again issues another token.
See [Configuration](configuration.md) for settings and startup.

## TCP Connection Authentication

The console's `/login` command performs HTTP login and TCP authentication
automatically. A raw client sends a [framed request](protocol.md#framing):

```json
{"type":"request","request_id":"auth-1","action":"authenticate","data":{"token":"<JWT>"}}
```

Tokens must be strings of 1–4096 bytes. The server validates signature, issuer,
type, subject, and timestamps. `iat` and `exp` are required nonnegative integers
that fit the server clock, with `iat` earlier than `exp`. Future `iat` or `nbf`
values are rejected. The subject must
be a positive unsigned 64-bit decimal ID without leading zeros. Decoded JWT JSON
also has the 64-level nesting limit. A database lookup confirms the user exists.

```json
{"type":"response","request_id":"auth-1","action":"authenticate","ok":true,"data":{"user_id":"1","expires_at":2000000000}}
```

`expires_at` is the verified expiration in Unix seconds. The Reactor binds the
session before scheduling offline replay and applying the next request.
Replay includes pending text and file notifications. See
[Messaging](messaging.md#acknowledgments-and-offline-replay).

Invalid, expired, incorrectly signed, or deleted-user tokens receive
`invalid_token`. Bad token fields receive `invalid_request`; database failures
receive `internal_error`. Responses and logs never include token contents.

## Session Rules

- `ping` and `authenticate` are available without a session. Other actions
  require an unexpired session, or return `authentication_required`.
- Each user has one online connection. A successful authentication replaces and
  closes the previous connection with reason `session_replaced`. Across
  connections, the last successful completion applied by the Reactor wins.
- A bound connection can refresh the same user's token. Failed attempts preserve
  its session. Switching users returns `already_authenticated`; reconnect to
  change accounts, even after expiration.
- Business requests check expiry when their worker begins. Online routing skips
  expired sessions. Expiration leaves TCP open for pings and authentication.
- Disconnect and shutdown remove online bindings. `/quit` disconnects; it does
  not revoke the JWT. Reconnecting requires authentication.

Workers verify JWTs and run SQL; only the Reactor modifies session maps. A late
authentication result for a closed connection cannot replace another session.
The Reactor checks expiration again before binding. Closing a connection drops
waiting requests, while already running work can still commit and notify
recipients. See [Threading](threading.md).

## HTTP Execution

cpp-httplib uses a separate fixed worker pool, defaulting to four workers, with
at most 128 accepted connections waiting. Excess connections are closed.
Read, write, and keep-alive timeouts are five seconds, with at most ten requests
per keep-alive connection. Each worker reuses its own database connection and
discards it after a database error. Writes are not automatically retried.

File endpoints share this pool and verify Bearer tokens independently of TCP
sessions. See [Files](files.md). HTTP request logs capture timestamp, client IP,
method, URI path, status, and error details while omitting credentials, bodies,
Authorization headers, and query strings. See [Logs](configuration.md#logs).
