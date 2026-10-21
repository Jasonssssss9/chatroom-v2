# JSON Protocol

This document defines the TCP message envelope and the behavior currently
implemented by the server. It also maps the original v1 operations to the planned
v2 features. Examples show JSON payloads; the four-byte frame header is omitted.

## 1. Current Scope

The server currently supports:

- Parsing and validating request envelopes.
- A correlated response to `ping`.
- JWT authentication with `authenticate` and Reactor-owned user sessions.
- Persisted private and multi-recipient messages with online notifications.
- Consistent errors for malformed JSON, invalid requests, and unsupported actions.
- Server-initiated `message` notifications.

The implemented actions are `ping`, `authenticate`, and `send_message`. Groups,
acknowledgments, offline replay, and file transfers are not implemented yet.
They do not return success placeholders.

## 2. Framing

TCP carries a stream of bytes rather than separate messages. Each protocol message
uses the framing already implemented by the network layer:

```text
+--------------------------+----------------------------+
| Payload length: 4 bytes  | UTF-8 JSON: length bytes    |
+--------------------------+----------------------------+
```

The header is an unsigned 32-bit integer in network byte order (big-endian). The
length counts the encoded JSON bytes, excluding the header. A payload may contain
at most 65,536 bytes. There is no trailing newline requirement, textual header,
blank-line separator, or JSON `Content-Length` field.

A message can arrive in several reads, and one read can contain several messages.
`FrameDecoder` handles both cases before JSON parsing begins. An oversized frame
closes the connection; the server does not try to parse or respond to it.

The framing library permits an empty payload, but it is not valid JSON and receives
an `invalid_json` response. The command-line client's one-line input format is a
terminal convenience, not a TCP message delimiter.

## 3. Message Roles

| `type` | Direction | Purpose |
| --- | --- | --- |
| `request` | Client to server | Ask the server to perform an operation |
| `response` | Server to client | Report the result of a request |
| `notification` | Server to client | Deliver an event independently of a request |

These roles replace v1's `REQ`, `RES`, and `INF`. Operation names such as `ping` and
`send_message` replace numeric operation codes. Names are case-sensitive.

The server currently accepts only `request` envelopes from clients. Sending a
`response` or `notification` to the server is an `invalid_request`. Future message
acknowledgments will be requests with an `ack` action, not response envelopes.

### Requests

```json
{
  "type": "request",
  "request_id": "ping-1",
  "action": "ping",
  "data": {}
}
```

| Field | Type | Rule |
| --- | --- | --- |
| `type` | String | Required; exactly `request` |
| `request_id` | String | Required; 1 to 64 UTF-8 bytes |
| `action` | String | Required; 1 to 64 UTF-8 bytes |
| `data` | Object | Optional; defaults to `{}` |

The root must be a JSON object. Arrays, numbers, strings, booleans, and `null` are
not request envelopes. An explicitly supplied `data` must be an object; `null`
does not mean the same thing as omitting it.

Requests may contain at most 64 nested objects and arrays. The root object counts
as level 1, and the `data` object counts as level 2. Empty containers count too;
brackets and braces inside strings do not. The limit applies to the entire
payload, including unknown fields. It is independent of the 65,536-byte frame limit.

A deeper payload is rejected during parsing with `invalid_request` and the message
`JSON nesting exceeds the 64-level limit`. Both correlation fields are `null`
because envelope validation has not run. The connection stays open, and later
requests are processed normally. Checking during parsing prevents deeply nested
data from reaching recursive JSON copying or serialization.

Unknown top-level fields are ignored. Each operation will validate its own data
fields when that operation is implemented. The envelope parser does not know
about passwords, recipients, or group membership.

Clients choose request IDs and should keep them distinct among outstanding
requests on a connection. The server copies the ID into its response; it does not
currently track duplicate request IDs. A request ID identifies an exchange, not a
persisted chat message or an authenticated user.

### Successful Responses

```json
{
  "type": "response",
  "request_id": "ping-1",
  "action": "ping",
  "ok": true,
  "data": {
    "pong": true
  }
}
```

`request_id` and `action` match the request. `ok` is a JSON boolean, not the string
`"true"`. Successful responses contain a `data` object and no `error` field. An
operation with no result data can return an empty object.

### Error Responses

```json
{
  "type": "response",
  "request_id": "r2",
  "action": "missing_action",
  "ok": false,
  "error": {
    "code": "unknown_action",
    "message": "Action is not supported"
  }
}
```

Errors contain an `error` object and no `data` field. Clients should branch on
`error.code`. The English `message` is for people and may become more descriptive
without changing the error code.

After parsing completes, valid correlation fields are retained even if another
part of the request is invalid. For example, this request has invalid data:

```json
{
  "type": "request",
  "request_id": "r3",
  "action": "ping",
  "data": null
}
```

Its response is:

```json
{
  "type": "response",
  "request_id": "r3",
  "action": "ping",
  "ok": false,
  "error": {
    "code": "invalid_request",
    "message": "data must be a JSON object"
  }
}
```

If JSON cannot be parsed, neither field is available:

```json
{
  "type": "response",
  "request_id": null,
  "action": null,
  "ok": false,
  "error": {
    "code": "invalid_json",
    "message": "Payload must contain valid JSON"
  }
}
```

A missing, incorrectly typed, empty, or oversized correlation field is represented
as JSON `null` in the response. The other field can still be retained. This avoids
reflecting large or invalid metadata into an error response.

### Notifications

The server uses notifications to deliver persisted messages independently of the
recipient's requests:

```json
{
  "type": "notification",
  "action": "message",
  "data": {
    "message_id": "105",
    "sender_id": "7",
    "client_message_id": "alice-1",
    "text": "Hello"
  }
}
```

A notification has an action and object data. It has no `request_id` or `ok` field
because it is not the result of a client request. Each delivery carries a
`message_id`; receivers can use it for deduplication. Network acknowledgments
will be added separately.

## 4. Implemented Actions

### ping

`ping` accepts omitted or object data and returns `{"pong":true}` in the response
data. It currently ignores extra data fields. It does not authenticate the connection,
query a database, or establish a heartbeat timeout.

A complete request that can be entered in the command-line client is:

```json
{"type":"request","request_id":"ping-1","action":"ping","data":{}}
```

The shorter form is also valid:

```json
{"type":"request","request_id":"ping-2","action":"ping"}
```

Each admitted request normally produces one response. Multiple requests can use
the same TCP connection. If a request-capacity limit is exceeded, the server closes
the connection and may discard pending replies; it does not queue an extra error
response. See [Request Capacity](threading.md#request-capacity). The client displays
responses with a `<` prefix; that prefix is not part of the wire message.

### authenticate

Obtain a token through HTTP login, then send:

```json
{"type":"request","request_id":"auth-1","action":"authenticate","data":{"token":"<JWT>"}}
```

`token` must contain 1 to 4,096 bytes. Success returns
`{"user_id":"1"}` in response data. The identity is verified on a worker and
bound on the Reactor before the next request starts. Every TCP action other than
`ping` and `authenticate` requires an unexpired session. A successful
authentication on another connection replaces the user's previous connection.
The same connection can refresh its own identity; changing users requires a new
connection. Failed authentication preserves any existing session.

See [Authentication](authentication.md#tcp-connection-authentication) for token
claims, expiration, duplicate login, and disconnect behavior.

### send_message

Send to one or more user IDs after authentication:

```json
{
  "type": "request",
  "request_id": "send-1",
  "action": "send_message",
  "data": {
    "client_message_id": "alice-1",
    "recipients": ["2", "3"],
    "text": "Hello everyone"
  }
}
```

| Field | Type | Rule |
| --- | --- | --- |
| `client_message_id` | String | Required; 1 to 64 UTF-8 bytes; unique per sender for each new message |
| `recipients` | Array | Required; 1 to 64 user ID strings before deduplication |
| `text` | String | Required; 1 to 4,096 UTF-8 bytes |

Each user ID is a positive unsigned 64-bit decimal string with no leading zeros,
sign, or whitespace. Duplicate recipients are merged; their order does not matter.
Sending to yourself is allowed. All recipients must exist, including offline users.
If any recipient is missing, the entire request receives `invalid_recipient` and
no message or delivery is created. Strings are used as supplied, without trimming.
Unknown data fields are ignored. A supplied `sender_id` never overrides the
authenticated session.

The worker saves one message and its recipient delivery rows in a transaction.
Only after commit does it return success:

```json
{"type":"response","request_id":"send-1","action":"send_message","ok":true,"data":{"message_id":"105"}}
```

The Reactor sends the `message` notification shown above to recipients with an
unexpired online session. Their current connections are resolved when the result
arrives, rather than when the request was received. A sender included in
`recipients` receives its response before its own notification. Other notifications
can interleave with responses, so clients must distinguish `type` and use request
IDs for correlation.

A success response confirms persistence, not receipt or reading. Offline users,
disconnects, or output-buffer failures leave the delivery pending in MySQL.
Automatic replay and `ack` are not implemented yet. If the sender disconnects
while its task runs, a committed message can still notify online recipients.

For a retry, reuse `client_message_id`, text, and the same recipient set. The
stored `message_id` is returned without extra database deliveries; online
recipients may receive the notification again. Reusing that client ID with changed
text or recipients receives `message_conflict`. Use a new client ID for a new
message. Different senders may use the same client ID independently.

See [Messaging](messaging.md) for a two-client walkthrough.

## 5. Current Errors and Connection Behavior

| Code | Cause | Connection |
| --- | --- | --- |
| `invalid_json` | Malformed JSON or invalid UTF-8 | Kept open |
| `invalid_request` | Excessive JSON nesting, wrong root, type, metadata, or data type | Kept open |
| `invalid_token` | Invalid or expired JWT, or token user no longer exists | Kept open |
| `already_authenticated` | A bound connection attempts to change users | Kept open |
| `authentication_required` | Action requires an unexpired session | Kept open |
| `invalid_recipient` | At least one recipient does not exist | Kept open |
| `message_conflict` | A sender reuses a client message ID with different text or recipients | Kept open |
| `unknown_action` | Authenticated request with an unsupported action | Kept open |
| `internal_error` | Unexpected application-handler or response serialization failure | Kept open |
| `response_too_large` | Serialized response or notification exceeds 65,536 bytes | Kept open |

Envelope validation happens before action dispatch. Within an object, the parser
checks `type`, `request_id`, `action`, then `data`, and reports the first failing
rule. Correlation metadata is collected before these checks. Authentication input
errors, including missing, non-string, empty, or oversized tokens, use
`invalid_request`. Unsupported actions on unauthenticated or expired sessions use
`authentication_required` before action dispatch.

After an error, the next complete frame is a new request and is processed normally.
Malformed JSON does not corrupt the framing of later requests. A worker replaces
an oversized response with a small `response_too_large` error carrying the request
ID and action. The same check applies to successful responses, returned application
errors, and responses built from a thrown `ProtocolError`. The limit applies to
serialized JSON bytes, including the envelope and escaped characters; exactly
65,536 bytes is allowed. Later requests on the connection continue normally.
Handlers returning large result sets should use pagination rather than truncating
JSON.

Transport failures, oversized incoming frames, and output-buffer limits still
follow the connection-closing rules in [Networking](networking.md).

If the client sends FIN partway through a frame header or payload, the server logs
`truncated_frame` and discards that incomplete frame. Complete requests preceding
it are still processed and replied to before the connection closes. There is no
JSON error response for the partial frame; `truncated_frame` is a transport log code
and close reason, not an application error code. FIN between complete frames is
an ordinary half-close.

## 6. Mapping v1 Features to v2

The [v1 request dispatcher](https://github.com/Jasonssssss9/chatroom-server-v1/blob/main/protocol.cpp)
used numeric codes and header fields. The table below keeps its user-facing
features while assigning readable operation names and the appropriate transport.

HTTP registration, login, TCP authentication, and direct or multi-recipient sends
are implemented. See [Authentication](authentication.md) and [Messaging](messaging.md).
Logout, groups, acknowledgments, and file transfers remain planned.

| v1 operation | v2 operation | Transport |
| --- | --- | --- |
| `010`: register | `POST /api/users` | HTTP |
| `020`: login | `POST /api/auth/token`, followed by TCP `authenticate` | HTTP and TCP |
| `030`: logout | `logout` | TCP |
| `110`: direct or multi-recipient message | `send_message` | TCP |
| `210`: create group | `create_group` | TCP |
| `220`: group message | `send_group_message` | TCP |
| `310`: upload file | `POST /api/files` | HTTP |
| `330`: download file | `GET /api/files/{id}` | HTTP |
| `INF` notifications | `message`, `group_created`, or `file_available` notifications | TCP |

The action data is intentionally small:

| Action | Planned data fields |
| --- | --- |
| `authenticate` (implemented) | `token` |
| `send_message` (implemented) | `client_message_id`, `recipients` (array of user ID strings), `text` |
| `create_group` | `name`, `members` (array of user ID strings) |
| `send_group_message` | `client_message_id`, `group_id`, `text` |
| `ack` | `message_id` |
| `logout` | Empty object |

The remaining payload fields will be implemented and validated with their business services.
The server obtains the sender from the authenticated connection, not a
client-supplied sender field. IDs are represented as strings in the protocol.
File bytes will travel over HTTP; TCP notifications will reference the file ID.

The three message-related IDs have different jobs:

| ID | Chosen by | Purpose |
| --- | --- | --- |
| `request_id` | Client | Match a response to a request |
| `client_message_id` | Sending client | Recognize retries of the same outgoing message |
| `message_id` | Server | Identify a persisted delivery for acknowledgment and deduplication |

Successful sends confirm that the server has persisted the message. Receiver
acknowledgments will be separate `ack` requests.

## 7. Code Layout and Extension

```text
FrameDecoder extracts a payload
    -> RequestDispatcher queues it by connection ID
    -> a worker parses the envelope and calls RequestHandler
    -> the worker serializes the response and posts a completion
    -> the Reactor applies authentication, queues responses and notifications, and starts the next request
```

`common/protocol.h` defines `Request`, `ProtocolError`, and message builders.
`server/src/request_handler.cpp` contains action dispatch. The socket code has no
knowledge of action names or JSON fields.

Adding a business operation means implementing its service, validating its data
in the handler, returning the standard response, and adding tests and examples.
There is no handler registration framework or separate schema language. Builders
accept trusted application data rather than revalidating server-generated values.

Handlers run on the fixed worker pool. Requests on one connection execute in order;
different connections can execute concurrently. Completions are applied by the
Reactor. See [Worker Pool and Request Handoff](threading.md) for ownership,
disconnects, and shutdown behavior.
