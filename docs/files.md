# File Transfers

HTTP carries file bytes. TCP carries a small `file_available` notification with
the file ID, name, size, and sender. File contents live on disk; MySQL stores
metadata, download permissions, and notification deliveries.

## Setup

For an existing database, apply the one-time migration described in
[Database](database.md#upgrading-an-existing-database). New databases use the
current `sql/schema.sql` directly.

The JSON configuration accepts:

```json
{
  "files": {
    "directory": "data/files",
    "max_size_bytes": 10485760
  }
}
```

The directory is relative to the server's working directory unless absolute. It
is created on the first valid upload and is ignored by Git at the default path.
Keep this directory across server restarts. The size limit defaults to 10 MiB;
configuration accepts 1 byte through 1 GiB. Empty files are allowed. The existing
4 KiB authentication-body limit remains independent of the upload limit.

## Console Commands

Log in first and wait for `Authenticated as user ...`:

```text
/login alice example-password
/upload 2,3 "./notes final.pdf"
```

The sender sees `Uploaded file 42.` Each selected recipient receives:

```text
[file 42, message 105] user 1: notes final.pdf (12345 bytes). Use /download 42 <path>.
```

The recipient can save it with:

```text
/download 42 "./received notes.pdf"
```

Downloads require a new local path and never overwrite an existing file. The
parent directory must already exist. An unsuccessful download removes its partial
output. Neither the original file name nor a server storage key chooses the local
destination. File transfers run on the input thread; the receiver continues
displaying chat and sending ACKs during transfers.

The console retains a JWT in memory only after the matching TCP authentication
response succeeds. A rejected account switch leaves the previous token in place.
Raw JSON authentication does not configure the console's HTTP file client.

## Upload API

`POST /api/files` requires:

- `Authorization: Bearer <JWT>` for an existing user.
- `Content-Type: application/octet-stream`.
- `Content-Length`, including zero for an empty file. Chunked uploads are not
  accepted.
- `X-File-Metadata`: a JSON object containing `name` and `recipients`.

The body contains the file bytes directly, without multipart encoding or base64:

```bash
curl -i http://127.0.0.1:8080/api/files \
  -H "Authorization: Bearer $CHATROOM_ACCESS_TOKEN" \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-File-Metadata: {"name":"notes.txt","recipients":["2","3"]}' \
  --data-binary @notes.txt
```

Metadata is limited to 4 KiB and 64 JSON container levels. `name` contains 1 to
255 UTF-8 bytes; path separators, control characters, `.` and `..` are rejected.
Use JSON Unicode escapes when an HTTP client cannot send non-ASCII header values.
`recipients` contains 1 to 64 positive user ID strings before deduplication. All
users must exist; one batch query checks them. The sender may be a recipient.
This API shares with selected users, rather than accepting a group ID.

Success is HTTP 201 with `Location: /api/files/42`:

```json
{"file_id":"42","message_id":"105","name":"notes.txt","size_bytes":12345}
```

The server chooses a random 128-bit storage key and creates a new file with
exclusive ownership and mode `0600`. Original names never become disk paths.
Uploads use a streaming reader, and the client uses an 8 KiB file-body provider;
neither side loads the entire file into a string. Files are stored as
`application/octet-stream`, without MIME detection.

## Download API

`GET /api/files/{id}` requires the same Bearer authorization. Only the owner and
the file's selected recipients can download. Other users and unknown file IDs
receive the same HTTP 404 response. Download access does not require an active
TCP connection or an unacknowledged notification.

```bash
curl --fail http://127.0.0.1:8080/api/files/42 \
  -H "Authorization: Bearer $CHATROOM_ACCESS_TOKEN" \
  --output received-notes.txt
```

Success is HTTP 200 with the binary body, `Content-Length`,
`Content-Type: application/octet-stream`, `Cache-Control: no-store`,
`X-Content-Type-Options: nosniff`, and an attachment `Content-Disposition` header.
The original UTF-8 name is percent-encoded in `filename*`. The server supplies
bytes through an 8 KiB file-body provider. A missing disk file receives HTTP 404;
a size mismatch with its metadata is an internal error.

## Notifications and Persistence

```json
{"type":"notification","action":"file_available","data":{"message_id":"105","file_id":"42","sender_id":"1","name":"notes.txt","content_type":"application/octet-stream","size_bytes":12345}}
```

The server finishes and synchronizes the disk file before starting the database
write. One transaction creates the file metadata, permissions, a `messages` row
referencing `file_id`, and its `message_deliveries` rows. File notification rows
have a NULL `client_message_id`; text-message retry keys remain unchanged.

Only after commit does the HTTP worker post a short callback to the Reactor.
The Reactor resolves the recipients' current sessions and sends the notification.
Workers do not access TCP sockets or session maps. Offline users receive the
same notification after TCP authentication, through the existing bounded replay.

Clients send `ack_message` with the notification's **message ID**, not its file ID.
ACK means the notification was accepted; it does not mean the file was downloaded.
The console deduplicates file notices and acknowledges every copy, just as it does
for text messages. Missing ACKs, reconnects, and replay batches have the same
behavior described in [Messaging](messaging.md).

Incomplete uploads and ordinary database failures remove the newly written disk
file. After a successful commit, losing the HTTP response does not remove it or
its notification. Uploads are not automatically retried or deduplicated: repeating
an uncertain upload can create another file.

MySQL and the filesystem are separate storage systems. A process crash between
disk creation and database commit can leave an orphan file, and an uncertain
database commit outcome may require reconciliation. There is no background
cleanup or distributed transaction. Back up the database and storage directory
together. The per-file limit does not impose a total disk quota or retention policy.

## Errors and Verification

Errors use the existing HTTP JSON error format. Relevant statuses are 400 for
invalid metadata or recipients, 401 for missing or invalid tokens, 404 for an
unavailable file, 411 for missing upload length, 413 for oversized content, 415
for the wrong media type, and 500 for storage or database failures.

Catch2 tests cover binary and empty files, exact size limits, invalid metadata,
authorization, disk cleanup after incomplete uploads and failed transactions,
online notices, replay without ACK, mixed text/file replay, client streaming,
local overwrite prevention, duplicate ACKs, and login-token handoff.
Additional regression cases cover metadata limits, unavailable storage, stored-size
mismatches, preservation after a post-commit failure, bounded client responses,
and download cancellation followed by a successful retry. The real-process
[integration walkthrough](testing.md#real-server-and-client-processes) checks
console transfers and pending file recovery after both normal and forced shutdown.

```bash
./build/tests/chatroom_tests "[file]"
CHATROOM_DB_NAME=chatroom_test ./build/tests/chatroom_database_tests "[file]"
```
