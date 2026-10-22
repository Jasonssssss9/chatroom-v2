# File Transfers

HTTP streams file bytes; TCP carries a `file_available` notice. File contents
live on disk. MySQL stores metadata, download permissions, and notification
deliveries.

## Configuration and Console

The default `files.directory` is `data/files`, relative to the server's working
directory. It is created on the first valid upload. Keep it across restarts.
`files.max_size_bytes` defaults to 10 MiB and accepts 1 byte–1 GiB; empty files
are allowed. See [Configuration](configuration.md) and, for older databases,
[Migration](database.md#upgrading-an-existing-database).

After login succeeds:

```text
/upload <recipient-id[,recipient-id...]> "<local-path>"
/download <file-id> "<new-local-path>"
```

The sender sees the file ID; recipients see its ID, message ID, sender, name, and
size. Downloads require an existing parent directory and never overwrite a file.
Failures remove partial output. Transfers run on the input thread while the
receiver continues displaying messages and sending ACKs.

The console retains an HTTP JWT only after the matching TCP authentication
succeeds. Raw JSON authentication does not configure its HTTP file client.
See [Client](client.md).

## Upload API

`POST /api/files` requires:

| Header | Value |
| --- | --- |
| `Authorization` | `Bearer <JWT>`, for an existing user |
| `Content-Type` | `application/octet-stream` |
| `Content-Length` | Exact byte length, including zero for an empty file |
| `X-File-Metadata` | JSON object containing `name` and `recipients` |

The body is the file itself, without multipart encoding or base64. Chunked
uploads are not accepted. With curl installed, a valid token in
`CHATROOM_ACCESS_TOKEN`, an existing recipient, and a local `notes.txt`:

```bash
curl -i http://127.0.0.1:8080/api/files \
  -H "Authorization: Bearer $CHATROOM_ACCESS_TOKEN" \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-File-Metadata: {"name":"notes.txt","recipients":["2"]}' \
  --data-binary @notes.txt
```

Replace the recipient ID with an actual user ID. Metadata is limited to 4096 bytes
and 64 nested containers. The name contains 1–255 UTF-8 bytes; `.`, `..`,
path separators, and control characters are rejected. Use JSON Unicode escapes
if an HTTP client cannot send non-ASCII header values.

Recipients are 1–64 positive user ID strings before deduplication. All must exist;
one ID-only query checks them. The sender may be included. The endpoint accepts
selected users, not a group ID.

Success is HTTP 201 with `Location: /api/files/42`:

```json
{"file_id":"42","message_id":"105","name":"notes.txt","size_bytes":12345}
```

The server generates a random 128-bit storage key and exclusively creates a file
with mode `0600`. Original names never become storage paths. Uploads stream
through the HTTP reader; the console's provider reads 8 KiB at a time. Neither
loads the entire file into memory. Stored content type is
`application/octet-stream`; there is no MIME detection.

## Download API

`GET /api/files/{id}` requires Bearer authentication. Only the owner and selected
recipients can download. Unknown files and unauthorized users receive the same
HTTP 404 response. Access is independent of TCP connection state and notification
ACKs.

```bash
curl --fail http://127.0.0.1:8080/api/files/42 \
  -H "Authorization: Bearer $CHATROOM_ACCESS_TOKEN" \
  --output received-notes.txt
```

Use the actual file ID. HTTP 200 carries the binary body, `Content-Length`,
`Content-Type: application/octet-stream`, `Cache-Control: no-store`,
`X-Content-Type-Options: nosniff`, and attachment `Content-Disposition`.
The original UTF-8 name is percent-encoded in `filename*`.

The server and console stream downloads in 8 KiB chunks. A missing disk file
returns 404; a size mismatch with stored metadata returns an internal error.
The console prevents local overwrites; curl's output handling is independent.

## Notifications and Persistence

```json
{"type":"notification","action":"file_available","data":{"message_id":"105","file_id":"42","sender_id":"1","name":"notes.txt","content_type":"application/octet-stream","size_bytes":12345}}
```

The save order is:

1. Stream the upload to a newly created disk file, then finish and synchronize it.
2. Commit metadata, permissions, a message referencing the file, and recipient
   deliveries in one MySQL transaction.
3. Post the notice to the Reactor, which resolves current online recipients.

File notice rows have NULL `client_message_id`; text retry keys are unaffected.
Offline recipients receive the same notice through bounded replay. Clients ACK
its **message ID**, not its file ID. ACK means the notice was accepted, not that
the file was downloaded. Console deduplication and ACK handling are shared with
[Messaging](messaging.md#acknowledgments-and-offline-replay).

Incomplete uploads and ordinary pre-commit database failures remove the new disk
file. After commit, losing the HTTP response or notification leaves the saved
file and pending deliveries intact. Uploads have no idempotency key or automatic
retry; repeating an uncertain upload can create another file.

MySQL and the filesystem do not share a transaction. A process crash between disk
creation and database commit can leave an orphan file; an uncertain commit outcome
may need reconciliation. There is no background orphan cleanup, total disk quota,
or retention policy. Back up the database and storage directory together.

## Errors and Tests

Errors use `{"error":{"code":"...","message":"..."}}`. Relevant statuses are
400 for invalid metadata or recipients, 401 for authentication failure, 404 for an
unavailable file, 411 for missing upload length, 413 for oversized content, 415
for an unsupported media type, and 500 for storage or database failure.

See [Testing](testing.md) for Catch2 file cases, isolated database setup, and
real-client transfers with recovery after normal and forced server shutdown.
