# TCP JSON Protocol

This reference describes the implemented TCP protocol. Examples show JSON payloads
without the four-byte frame header. HTTP registration, login, and file bytes use
separate endpoints; see [Authentication](authentication.md) and [Files](files.md).

## Framing

```text
+-----------------------------+--------------------------+
| Payload length: 4 bytes     | UTF-8 JSON: length bytes |
+-----------------------------+--------------------------+
```

Length is an unsigned 32-bit integer in network byte order, excluding the header.
The maximum payload is 65536 bytes. There is no newline delimiter or textual
header. TCP reads may split or combine frames; the decoder handles both.

Oversized frames close the connection without a JSON response. An empty frame
is permitted by the framing layer but receives `invalid_json`. The console
accepts one JSON request per input line and adds framing itself.

## Envelopes

### Request

```json
{"type":"request","request_id":"ping-1","action":"ping","data":{}}
```

| Field | Rule |
| --- | --- |
| `type` | Required string, exactly `request` |
| `request_id` | Required string, 1–64 UTF-8 bytes |
| `action` | Required string, 1–64 UTF-8 bytes; case-sensitive |
| `data` | Optional object, defaults to `{}`; explicit null is invalid |

The root must be an object. Unknown fields are ignored; handlers validate their
own action fields. A client-supplied sender, owner, or user field cannot override
the authenticated session.

JSON may contain at most 64 nested objects and arrays. The root is level 1 and
`data` is level 2. Empty containers count, including those in unknown fields;
braces inside strings do not. Excessive nesting returns `invalid_request`
before envelope validation, with both correlation fields null.

Keep request IDs distinct among outstanding requests on a connection. The server
echoes them without tracking uniqueness. They are not idempotency keys.

### Response

Success:

```json
{"type":"response","request_id":"ping-1","action":"ping","ok":true,"data":{"pong":true}}
```

Failure:

```json
{"type":"response","request_id":"r2","action":"missing_action","ok":false,"error":{"code":"unknown_action","message":"Action is not supported"}}
```

Success has object `data` and no `error`; failure has object `error` and no
`data`. Clients should branch on `error.code`; the English message is for
display and may change.

Valid correlation fields survive other envelope errors. A missing, incorrectly
typed, empty, or oversized field becomes null independently of the other field.
If parsing fails, neither field is available:

```json
{"type":"response","request_id":null,"action":null,"ok":false,"error":{"code":"invalid_json","message":"Payload must contain valid JSON"}}
```

Envelope checks run in this order: root, `type`, `request_id`, `action`,
then `data`. Metadata is collected before field checks.

### Notification

```json
{"type":"notification","action":"message","data":{"message_id":"105","sender_id":"1","client_message_id":"alice-1","text":"Hello"}}
```

Notifications have an action and object data, with no `request_id` or `ok`.
They can interleave with responses. Only requests are accepted from clients;
ACKs are requests, not response envelopes.

## Identifiers

| Field | Chosen by | Purpose |
| --- | --- | --- |
| `request_id` | Client | Correlate one request and response |
| `client_message_id` | Sending client | Retry one immutable text message |
| `message_id` | Server | Identify a persisted message for ACK and deduplication |
| `file_id` | Server | Identify a downloadable file |
| `user_id`, `sender_id`, `owner_id`, `group_id` | Server | Identify users and groups |

Persistent IDs are positive unsigned 64-bit decimal strings: no signs, spaces,
zero, or leading zeros. This avoids JSON number precision loss. A file notice's
`message_id` is distinct from its `file_id`.

## Actions

Only `ping` and `authenticate` are available without an unexpired session.
All other actions use the identity bound to the connection. Requests execute and
respond in order on one connection; different connections have no global order.

The examples below show `data` unless a full envelope is needed.

### ping

Data may be omitted or `{}`. Success returns `{"pong":true}`. Extra fields
are ignored. No database operation is performed.

```json
{"type":"request","request_id":"ping-1","action":"ping"}
```

Heartbeat timing and idle cleanup are described in [Keepalive](keepalive.md).

### authenticate

Request data:

```json
{"token":"<JWT>"}
```

The token must be a string of 1–4096 bytes. After JWT validation and a user lookup,
success returns:

```json
{"user_id":"1","expires_at":2000000000}
```

`expires_at` is Unix seconds. The session is bound before the next request and
before offline replay. A new connection replaces the user's old connection.
Same-user refresh is allowed; switching users on a bound connection returns
`already_authenticated`. See
[Session Rules](authentication.md#session-rules).

### send_message

```json
{"client_message_id":"alice-1","recipients":["2","3"],"text":"Hello everyone"}
```

The key contains 1–64 bytes, text contains 1–4096 UTF-8 bytes, and recipients
contain 1–64 user ID strings before deduplication. All users must exist. Repeated
IDs merge; self-send is allowed.

Success returns `{"message_id":"105"}` after the message and deliveries commit.
An exact retry with the same sender, key, text, and recipient set returns the
original ID. Changed content or recipients returns `message_conflict`.
Online recipients receive `message`; offline deliveries remain pending.
See [Messaging](messaging.md).

### ack_message

```json
{"message_id":"105"}
```

Success echoes `{"message_id":"105"}`. The session user must own that delivery.
Repeated ACKs succeed without changing the original timestamp; missing or
another user's deliveries return `invalid_message`.

ACK means notification acceptance, not a read receipt or completed file download.
Acknowledge both text and file notices by message ID, including duplicates.

### create_group

```json
{"name":"Study group","members":["2","3"]}
```

The name contains 1–128 UTF-8 bytes. Members are optional, with 0–64 ID strings
before deduplication. The creator is added automatically; the final set may
contain at most 64 users, all of whom must exist.

Success returns `{"group_id":"42"}` after committing the group and memberships.
Other online members receive `group_created`. Each call creates a new group.

### list_groups

Use `{}` for the first page, or `{"after_group_id":"42"}` for the next.

```json
{"groups":[{"group_id":"42","owner_id":"1","name":"Study group"}],"next_after_group_id":null}
```

Only the session user's current groups are returned, ordered by ID, at most 64
per page. When another page exists, `next_after_group_id` is the last returned
ID; otherwise it is null. The cursor is exclusive. See
[Group Listing](groups.md#list_groups).

### get_group_members

Request `{"group_id":"42"}`; success returns
`{"group_id":"42","members":["1","2","3"]}`, ordered by user ID.

The caller must be a current member. Missing groups and nonmembership both
return `not_group_member`.

### send_group_message

```json
{"client_message_id":"alice-group-1","group_id":"42","text":"Hello group"}
```

Key and text limits match `send_message`. The sender must be a current member.
Success returns `{"message_id":"105","group_id":"42"}`. Every other member
gets a delivery; an owner-only group can persist a message with no deliveries.

Private and group messages share one retry-key namespace per sender. Exact group
retries retain the original recipient snapshot; changed text, group, or message
kind returns `message_conflict`. See [Groups](groups.md).

## Notification Actions

| Action | Data | ACK and replay |
| --- | --- | --- |
| `message` | `message_id`, `sender_id`, `client_message_id`, `text`; also `group_id` for group chat | Yes |
| `file_available` | `message_id`, `file_id`, `sender_id`, `name`, `content_type`, integer `size_bytes` | Yes, using message ID |
| `group_created` | `group_id`, `owner_id`, `name`, sorted `members` | Live only; no ACK or replay |

Pending deliveries replay after the authentication response in batches of at most
16 notices and 128 KiB, including frame headers. All current batch IDs must be
ACKed before the next query. Ordinary requests continue while ACKs are pending.

Notifications may repeat after retries, reconnects, or overlap with online routing.
Deduplicate by message ID and ACK every copy. There is no timed resend;
reauthentication queries remaining pending rows. Replay failure closes the
connection without deleting pending deliveries. See
[Offline Replay](messaging.md#acknowledgments-and-offline-replay).

## Errors and Connection Behavior

The following application errors keep the connection open:

| Code | Cause |
| --- | --- |
| `invalid_json` | Malformed JSON or invalid UTF-8 |
| `invalid_request` | Invalid envelope, fields, lengths, IDs, or nesting |
| `invalid_token` | Invalid or expired JWT, or its user no longer exists |
| `already_authenticated` | Attempt to switch users on a bound connection |
| `authentication_required` | No unexpired session for the action |
| `invalid_recipient` | A selected recipient or group member does not exist |
| `message_conflict` | Retry key reused for another message |
| `invalid_message` | No matching delivery belongs to the caller |
| `not_group_member` | Missing group or caller is not a member |
| `unknown_action` | Authenticated request uses an unsupported action |
| `internal_error` | Unexpected handler, database, or serialization failure |
| `response_too_large` | Serialized response or notification exceeds 65536 bytes |

Envelope validation precedes dispatch. Unsupported actions without a valid
session receive `authentication_required` before `unknown_action`.

Every response, including exception-generated errors, passes the same serialized
size check. Oversized results become a small correlated `response_too_large`
error; their notification effects are not routed. The next complete frame can
still be processed. Exactly 65536 bytes is allowed; JSON escaping counts.

Transport errors, oversized incoming frames, output overflow, request overload,
idle expiry, and session replacement close connections. A FIN inside a frame
logs `truncated_frame` without a JSON response, while complete preceding
requests still finish. See [Networking](networking.md) and
[Threading](threading.md#request-capacity).

There is no `logout` action. Disconnect removes the online binding without
revoking the JWT.
