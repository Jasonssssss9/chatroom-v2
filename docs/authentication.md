# Authentication

Registration, login, and file transfers run in C++ in the same process as TCP. HTTP defaults to
`127.0.0.1:8080`; TCP remains on port 8081. cpp-httplib handles HTTP/1.1, OpenSSL
provides password hashing, and jwt-cpp signs JWTs.

## Setup

Import `sql/schema.sql` and configure MySQL as described in [Database](database.md).
Supply both secrets through the environment:

```bash
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_JWT_SECRET="$(openssl rand -hex 32)"
./build/server/chatroom_server
```

The JWT secret must contain at least 32 bytes. Keep it stable across restarts if
previously issued tokens should remain valid. The example above generates a new
secret each time. `.env` files are not loaded automatically.

Both listeners use `server.address`. `--http-port` and `--http-workers` override
HTTP settings; port 0 selects an available port, reported as `http_port` in the
`server_started` log. JSON settings are:

```json
{
  "http": {"port": 8080, "worker_threads": 4},
  "jwt": {"token_lifetime_seconds": 3600}
}
```

JWT lifetime must be from 1 to 86,400 seconds. Startup rejects missing or short
JWT secrets, failed database connections, and failed listener binds. This is a
local HTTP endpoint; use HTTPS termination when deploying over an untrusted network.

## API

Both endpoints accept POST with `Content-Type: application/json` and a UTF-8
JSON object containing `username` and `password`. A charset parameter is accepted.
Unknown fields are ignored. Bodies are limited to 4,096 bytes and 64 nested
objects or arrays, counting the root as level 1.

File endpoints use `Authorization: Bearer <JWT>` and check that its user still
exists. They share the HTTP worker pool, while uploads have their own size limit
and streaming reader. See [File Transfers](files.md).

Usernames contain 1 to 64 UTF-8 bytes and are case-sensitive. Passwords contain 8
to 128 UTF-8 bytes. Values are used exactly as supplied, including spaces and
Unicode. Neither value is reflected in error messages.

Register a user:

```bash
curl -i http://127.0.0.1:8080/api/users \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"example-password"}'
```

Success is HTTP 201, with a string ID:

```json
{"id":"1","username":"alice"}
```

Login:

```bash
curl -i http://127.0.0.1:8080/api/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"example-password"}'
```

Success is HTTP 200:

```json
{"access_token":"<JWT>","token_type":"Bearer","expires_in":3600}
```

Responses use JSON and `Cache-Control: no-store`. They do not use the TCP envelope
or its request ID. Errors have this shape:

```json
{"error":{"code":"invalid_credentials","message":"Invalid username or password"}}
```

| Status | Code | Meaning |
| --- | --- | --- |
| 400 | `invalid_json` | Malformed JSON or invalid UTF-8 |
| 400 | `invalid_request` | Wrong fields, byte limits, root type, or excessive nesting |
| 401 | `invalid_credentials` | Unknown username or incorrect password |
| 404 | `not_found` | Unknown endpoint |
| 405 | `method_not_allowed` | Known endpoint requires POST; includes `Allow: POST` |
| 409 | `username_taken` | Username already exists |
| 413 | `payload_too_large` | Body exceeds 4,096 bytes |
| 415 | `unsupported_media_type` | Content-Type is not application/json |
| 500 | `internal_error` | Database, hashing, or other unexpected failure |

The database's unique constraint handles concurrent registrations. Unknown-user
and wrong-password attempts get the same 401 body. Unknown-user attempts still
perform password verification against a dummy hash.

## Passwords and JWTs

OpenSSL scrypt uses `N=32768`, `r=8`, `p=3`, a random 16-byte salt, and a 32-byte
derived key. This is one of the configurations listed in the
[OWASP password storage guidance](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html).
Verification uses OpenSSL's constant-time comparison. Stored hashes have the
format `scrypt$32768$8$3$<salt-hex>$<key-hex>`; parameters are fixed by this
implementation. Raw passwords are never stored in MySQL.

JWTs use HS256 and include `typ=JWT`, `iss=chatroom-v2`, the string user ID in
`sub`, `iat`, and `exp`. Passwords and hashes are not claims. Refresh tokens and
revocation lists are outside the current scope. TCP validates these tokens before
binding connections to users.

## TCP Connection Authentication

The console's `/login <username> <password>` command performs HTTP login and TCP
authentication automatically. See [Command-Line Client](client.md). For a raw
protocol client, obtain the JWT through HTTP and send a framed JSON request:

```json
{"type":"request","request_id":"auth-1","action":"authenticate","data":{"token":"<JWT>"}}
```

The token must be a nonempty string of at most 4,096 bytes. Missing or incorrectly
typed input receives `invalid_request`. The server verifies the HS256 signature,
issuer, type, string subject, issued-at time, and expiration. `iat` and `exp` are
required integer timestamps; dates must fit the server's clock. Future `iat` or
`nbf` values are rejected. The subject must be a positive unsigned 64-bit user ID
in decimal, with no leading zeros. Decoded JWT JSON has the same 64-level nesting
limit as request JSON. A MySQL lookup confirms that the user still exists.

On success:

```json
{"type":"response","request_id":"auth-1","action":"authenticate","ok":true,"data":{"user_id":"1"}}
```

After binding the session, the server queries unacknowledged deliveries on a
worker and sends bounded batches of `message` notifications. The client must
acknowledge each delivery; the console does this automatically while stdin is
open. See [Messaging](messaging.md#acknowledgments-and-offline-replay).

Invalid, expired, or incorrectly signed tokens and missing users all receive
`invalid_token` with `Token is invalid or expired`. Token contents are never
included in responses or server logs. Database failures receive `internal_error`.

Session rules are deliberately small:

- `ping` and `authenticate` are available before authentication. Every other
  action requires an unexpired session; otherwise it receives
  `authentication_required`. Authenticated users can send messages, create groups,
  query their groups' members, and acknowledge deliveries. Group queries and
  sends also require current membership;
  unimplemented actions receive `unknown_action`.
- Each user has one online TCP connection. When a new authentication succeeds,
  the Reactor replaces the binding and immediately closes the previous connection
  with reason `session_replaced`. Requests on different connections have no global
  order; the last successful completion applied by the Reactor owns the session.
- The same connection may authenticate again for the same user, using a token
  obtained by logging in again. Failed attempts preserve the previous session.
  Changing users on a bound connection receives `already_authenticated`; reconnect
  to switch users, even if the previous session has expired.
- A session expires with its token. Business requests check expiry when their
  worker starts handling them, and online routing excludes expired sessions.
  There is no background timer that closes expired connections. They can still
  send `ping` or authenticate again.
- Disconnect and server shutdown remove the online binding. The server does not
  persist socket state. Reconnecting requires authentication again.

JWT verification and SQL run on TCP workers. Only the Reactor modifies session
maps, before it submits the connection's next request. If a connection disappears
while authentication runs, its late result cannot establish a session or replace
another connection. Expiration is checked again when applying authentication.
Closing a replaced connection drops waiting requests and direct responses; it
does not cancel business work already running on a worker. A committed message
can still notify its recipients. See [Threading](threading.md).

The user ID comes from the verified token, never a client-supplied `user_id` or
sender field. The sender's existence is checked at authentication rather than on
every request. Message handlers validate recipients separately. There is no token
revocation service.

## Threads and Logs

HTTP has its own fixed pool of four workers by default. Password hashing and SQL
run on these workers independently of the TCP Reactor and its request pool. Each
HTTP worker lazily creates and reuses its own database connection. A database
error discards that connection; a later request can reconnect. Writes are not
automatically replayed.

At most 128 accepted connections wait in the HTTP queue; excess connections are
closed. Read, write, and keep-alive timeouts are five seconds, with at most ten
requests per keep-alive connection. Stop joins HTTP workers before authentication
objects are destroyed. An unexpected HTTP listener failure stops the TCP event
loop and is reported by the process owner.

Each `http_request` log includes timestamp, client IP, HTTP method, URI path,
status, and structured response error details. Query strings, request bodies,
passwords, Authorization headers, and issued tokens are not logged. Database
errors additionally record the MySQL error number without SQL values.

## Testing

Password and token issuance tests run with the ordinary suite. HTTP integration
tests use a real listener and the isolated `chatroom_test` database:

```bash
cmake -S . -B build -DBUILD_DATABASE_TESTS=ON
cmake --build build -j2
CHATROOM_DB_NAME=chatroom_test ctest --test-dir build -L database --output-on-failure
```

See [Database Tests](database.md#integration-tests) for setup and reset behavior.
Test JWT secrets are supplied by the tests; the database password comes from
`CHATROOM_DB_PASSWORD`. Postman can use the same URLs and JSON bodies shown above.
TCP integration tests also exercise HTTP registration and login, framed TCP
authentication, pipelined requests, same-user refresh, connection replacement,
missing users, database failures, and shutdown cleanup. Ordinary session tests
cover late completions, expiry, and rejected authentication results.
