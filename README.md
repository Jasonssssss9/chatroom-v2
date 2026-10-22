# chatroom-v2

A C++17 chat application for Linux, with a command-line client and a server built on non-blocking TCP, epoll, and the Reactor pattern. It supports private messages, multi-recipient messages, group chat, and streamed file transfers. HTTP handles registration, login, and JWT issuance; MySQL stores users, groups, messages, and file metadata. Delivery acknowledgments and offline replay keep unconfirmed messages available after reconnecting.

## Run

Run the following commands from the repository root. The setup below targets Ubuntu 24.04, including WSL2. You need CMake 3.20+, MySQL 8.0+, and OpenSSL 3.0+.

### 1. Install dependencies

```bash
sudo apt update
sudo apt install build-essential cmake git pkg-config libmysqlclient-dev \
  libssl-dev openssl mysql-server mysql-client
sudo systemctl start mysql
```

If WSL does not use systemd, start MySQL with `sudo service mysql start` instead. CMake downloads pinned third-party dependencies from GitHub during the first configuration.

### 2. Set up MySQL

Choose a database password and replace the placeholder below. For an existing account, keep its current password and skip account creation.

```bash
sudo mysql <<'SQL'
CREATE DATABASE IF NOT EXISTS chatroom CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'chatroom'@'127.0.0.1'
  IDENTIFIED BY 'replace-with-your-password';
GRANT ALL PRIVILEGES ON chatroom.* TO 'chatroom'@'127.0.0.1';
SQL

mysql --protocol=TCP -h 127.0.0.1 -u chatroom -p chatroom < sql/schema.sql
```

If you have a database from an older version, check the [migration instructions](docs/database.md#upgrading-an-existing-database) before starting the server.

### 3. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DATABASE_TESTS=OFF
cmake --build build -j2
```

This produces separate executables: `build/server/chatroom_server` and `build/client/chatroom_client`.

### 4. Start the server

```bash
export CHATROOM_DB_HOST=127.0.0.1
export CHATROOM_DB_PORT=3306
export CHATROOM_DB_NAME=chatroom
export CHATROOM_DB_USER=chatroom
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_JWT_SECRET="$(openssl rand -hex 32)"
./build/server/chatroom_server
```

Wait for the `server_started` JSON log. The default listeners are TCP `127.0.0.1:8081` and HTTP `127.0.0.1:8080`. Stop the server with Ctrl+C. When restarting, reuse the same JWT secret to keep existing tokens valid. Run from the same directory to retain access to files under `data/files/`.

Use `--help` for options or `--config config/server.example.json` to load the example configuration. See [Configuration](docs/configuration.md) for all settings.

## Verify

### 1. Register and log in

Open two more terminals at the repository root. In Alice's terminal, create a sample file and start the client:

```bash
printf 'Hello from Alice.\n' > /tmp/chatroom-demo.txt
./build/client/chatroom_client
```

Enter these commands in the client:

```text
/register alice example-password
/login alice example-password
```

Start `./build/client/chatroom_client` in Bob's terminal, then enter:

```text
/register bob example-password
/login bob example-password
```

Registration prints each user's ID. If the accounts already exist, log in with their existing passwords. Wait for `Authenticated as user ...` before continuing. Replace every `<...>` placeholder below with the actual numeric ID; do not type the angle brackets.

### 2. Send messages and create a group

In Alice's client:

```text
/send <bob-id> Hello Bob
/send <alice-id>,<bob-id> Hello both
/create_group "Study group" <bob-id>
```

Bob receives the messages automatically. Use the group ID printed by `/create_group` in Alice's client:

```text
/send_group <group-id> Hello everyone
```

In Bob's client:

```text
/groups
/members <group-id>
/send_group <group-id> Hi Alice
```

### 3. Transfer a file

In Alice's client:

```text
/upload <bob-id> "/tmp/chatroom-demo.txt"
```

Use the file ID from Bob's notification:

```text
/download <file-id> "/tmp/chatroom-demo-received.txt"
```

The destination must not already exist. In another shell, verify the bytes:

```bash
cmp /tmp/chatroom-demo.txt /tmp/chatroom-demo-received.txt
```

No output means the files match.

### 4. Check offline delivery and restart recovery

Enter `/quit` in Bob's client, then send him another message from Alice. Restart Bob's client and run `/login bob example-password`. The pending message appears automatically; the client acknowledges it. Reconnect again to check that confirmed deliveries are no longer replayed.

Stop and restart the server in its original terminal with `./build/server/chatroom_server`, keeping the same environment and working directory. Restart both clients and log in again. `/groups` and `/download` should still find the saved group and file. Use a new download destination. `/help` lists all client commands; `/quit` exits.

### 5. Run automated tests

```bash
ctest --test-dir build --output-on-failure
```

This runs the ordinary Catch2 suite without MySQL. The [testing guide](docs/testing.md) covers the separate disposable database, full integration tests, restart checks, and a standalone client build. See the [documentation index](docs/README.md) for protocol, architecture, and performance details.
