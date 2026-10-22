#!/usr/bin/env python3
"""Exercise real server processes, MySQL, TCP peers, and the console client."""

import argparse
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import secrets
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Server:
    def __init__(self, executable, config, secret, log_path):
        environment = dict(os.environ, CHATROOM_JWT_SECRET=secret)
        self.log = log_path.open("wb")
        self.process = subprocess.Popen(
            [str(executable), "--config", str(config)], env=environment,
            stdout=subprocess.DEVNULL, stderr=self.log)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                for line in log_path.read_text().splitlines():
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue  # The listener may still be writing the last log line.
                    if record["event"] == "server_started":
                        self.tcp_port = record["context"]["port"]
                        self.http_port = record["context"]["http_port"]
                        return
                require(self.process.poll() is None, "Server failed during startup")
                time.sleep(0.01)
            raise RuntimeError("Timed out waiting for server startup")
        except BaseException:
            self.close()
            raise

    def stop(self, shutdown_signal):
        self.process.send_signal(shutdown_signal)
        status = self.process.wait(timeout=10)
        expected = 0 if shutdown_signal == signal.SIGTERM else -signal.SIGKILL
        require(status == expected, "Server exited with an unexpected status")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def http(server, method, path, body=None, token=None, metadata=None):
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    if metadata is not None:
        headers["X-File-Metadata"] = json.dumps(metadata, ensure_ascii=True)
    if isinstance(body, dict):
        body = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    elif body is not None:
        headers["Content-Type"] = "application/octet-stream"
    connection = HTTPConnection("127.0.0.1", server.http_port, timeout=5)
    try:
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def json_http(server, path, body, expected):
    status, data = http(server, "POST", path, body)
    require(status == expected, f"Unexpected status {status} from {path}")
    return json.loads(data)


class Peer:
    def __init__(self, server, token=None):
        self.socket = socket.create_connection(("127.0.0.1", server.tcp_port), timeout=5)
        if token:
            require(self.request("authenticate", {"token": token})["ok"], "TCP authentication failed")

    def read(self, length):
        data = bytearray()
        while len(data) < length:
            chunk = self.socket.recv(length - len(data))
            require(bool(chunk), "Peer closed before a complete frame arrived")
            data.extend(chunk)
        return data

    def receive(self):
        length = struct.unpack("!I", self.read(4))[0]
        require(length <= 65536, "Oversized response frame")
        return json.loads(self.read(length))

    def request(self, action, data=None):
        payload = json.dumps({"type": "request", "request_id": action,
                              "action": action, "data": data or {}}).encode()
        self.socket.sendall(struct.pack("!I", len(payload)) + payload)
        response = self.receive()
        require(response.get("type") == "response" and response.get("action") == action,
                f"Expected a response for {action}, got {response.get('action')}")
        return response

    def notice(self, action, message=None):
        notice = self.receive()
        require(notice.get("type") == "notification" and notice.get("action") == action,
                f"Expected a {action} notification")
        if message:
            require(notice["data"]["message_id"] == message, "Unexpected notification message ID")
        return notice["data"]

    def ack(self, message):
        require(self.request("ack_message", {"message_id": message})["ok"], "ACK failed")

    def empty_inbox(self):
        # Authentication replay is sequenced before this request on the same connection.
        require(self.request("ping")["data"]["pong"], "Expected an empty replay queue")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()


class Console:
    def __init__(self, executable, server):
        self.process = subprocess.Popen(
            [str(executable), "127.0.0.1", str(server.tcp_port), str(server.http_port)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.output = ""
        self.offset = 0

    def command(self, line):
        self.process.stdin.write((line + "\n").encode())
        self.process.stdin.flush()

    def expect(self, marker):
        deadline = time.monotonic() + 10
        while True:
            position = self.output.find(marker, self.offset)
            if position >= 0:
                self.offset = position + len(marker)
                return
            remaining = deadline - time.monotonic()
            require(remaining > 0, f"Timed out waiting for console output: {marker}")
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            require(bool(readable), f"No console output: {marker}")
            chunk = os.read(self.process.stdout.fileno(), 8192)
            require(bool(chunk), f"Console exited before displaying: {marker}")
            self.output += chunk.decode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()


def run_scenario(server_executable, client_executable, shutdown_signal):
    with tempfile.TemporaryDirectory(prefix="chatroom-e2e-") as temporary:
        directory = Path(temporary)
        config = directory / "server.json"
        config.write_text(json.dumps({"server": {"port": 0}, "http": {"port": 0},
            "files": {"directory": str(directory / "files"), "max_size_bytes": 131072}}))
        secret = secrets.token_hex(32)
        prefix = "e2e-" + secrets.token_hex(6)
        users, tokens, credentials = {}, {}, {}
        content = bytes(range(256)) * 256 + b"\x00final bytes\xff"
        with Server(server_executable, config, secret, directory / "first.log") as server:
            for name in ("alice", "bob", "carol", "outsider"):
                credentials[name] = {"username": prefix + "-" + name, "password": "example-password"}
                users[name] = json_http(server, "/api/users", credentials[name], 201)["id"]
                tokens[name] = json_http(server, "/api/auth/token", credentials[name], 200)["access_token"]
            json_http(server, "/api/users", credentials["alice"], 409)
            json_http(server, "/api/auth/token", dict(credentials["alice"], password="incorrect-password"), 401)
            with Peer(server, tokens["alice"]) as alice, Peer(server, tokens["bob"]) as bob, Peer(server) as outsider:
                alice.empty_inbox()
                bob.empty_inbox()
                require(outsider.request("list_groups")["error"]["code"] == "authentication_required",
                        "Unauthenticated group query succeeded")
                require(outsider.request("authenticate", {"token": tokens["outsider"]})["ok"], "Outsider login failed")
                outsider.empty_inbox()
                direct = {"client_message_id": "private-1", "recipients": [users["bob"]], "text": "Hello Bob"}
                private_id = alice.request("send_message", direct)["data"]["message_id"]
                require(bob.notice("message", private_id)["text"] == "Hello Bob", "Private message content changed")
                bob.ack(private_id)
                bob.ack(private_id)
                require(alice.request("send_message", direct)["data"]["message_id"] == private_id, "Retry created a new message")
                bob.notice("message", private_id)
                bob.ack(private_id)
                require(alice.request("send_message", dict(direct, text="Changed"))["error"]["code"] == "message_conflict",
                        "Conflicting retry succeeded")
                multi = {"client_message_id": "multi-1", "recipients": [users["bob"], users["carol"], users["bob"]],
                         "text": "Hello everyone"}
                multi_id = alice.request("send_message", multi)["data"]["message_id"]
                bob.notice("message", multi_id)
                bob.ack(multi_id)
                group = alice.request("create_group", {"name": "Study group", "members": [users["bob"], users["carol"]]})["data"]["group_id"]
                require(bob.notice("group_created")["group_id"] == group, "Wrong group creation notice")
                require(bob.request("get_group_members", {"group_id": group})["data"]["members"] ==
                        sorted([users["alice"], users["bob"], users["carol"]], key=int), "Wrong group members")
                grouped = {"client_message_id": "group-1", "group_id": group, "text": "Hello group"}
                group_id = alice.request("send_group_message", grouped)["data"]["message_id"]
                bob.notice("message", group_id)
                bob.ack(group_id)
                for action in ("get_group_members", "send_group_message"):
                    require(outsider.request(action, grouped)["error"]["code"] == "not_group_member", "Group permission failed")
                require(outsider.request("ack_message", {"message_id": private_id})["error"]["code"] == "invalid_message",
                        "Outsider acknowledged another user's message")
                metadata = {"name": "notes final.bin", "recipients": [users["bob"], users["carol"]]}
                status, uploaded = http(server, "POST", "/api/files", content, tokens["alice"], metadata)
                require(status == 201, "File upload failed")
                uploaded = json.loads(uploaded)
                file_id, file_message = uploaded["file_id"], uploaded["message_id"]
                bob.notice("file_available", file_message)
                require(http(server, "GET", "/api/files/" + file_id, token=tokens["alice"]) == (200, content), "Owner download changed bytes")
                require(http(server, "GET", "/api/files/" + file_id, token=tokens["outsider"])[0] == 404, "Outsider downloaded a file")
                require(http(server, "GET", "/api/files/" + file_id)[0] == 401, "Anonymous download succeeded")
                # No ACK: reconnecting must replay this same file notification.
            with Peer(server, tokens["bob"]) as bob:
                bob.notice("file_available", file_message)
            with Console(client_executable, server) as console:
                console.command(f"/login {credentials['alice']['username']} example-password")
                console.expect("Authenticated as user " + users["alice"] + ".")
                server.stop(shutdown_signal)
                console.expect("Connection closed by server.")
                console.expect("Disconnected.")
                require(console.process.wait(timeout=5) == 0, "Console failed to handle server shutdown")

        with Server(server_executable, config, secret, directory / "second.log") as server:
            require(http(server, "GET", "/api/files/" + file_id, token=tokens["alice"]) == (200, content),
                    "Stored file bytes or owner permissions lost after restart")
            require(http(server, "GET", "/api/files/" + file_id, token=tokens["outsider"])[0] == 404,
                    "Outsider gained file access after restart")
            with Peer(server, tokens["alice"]) as alice:
                alice.empty_inbox()
                require(alice.request("list_groups")["data"]["groups"][0]["group_id"] == group, "Group lost after restart")
                require(alice.request("send_message", direct)["data"]["message_id"] == private_id, "Private retry key lost after restart")
                require(alice.request("send_group_message", grouped)["data"]["message_id"] == group_id, "Group retry key lost after restart")
                with Peer(server, tokens["carol"]) as carol:
                    for action, message in (("message", multi_id), ("message", group_id), ("file_available", file_message)):
                        carol.notice(action, message)
                        # Drain the initial replay batch before receiving ACK responses.
                    for message in (multi_id, group_id, file_message):
                        carol.ack(message)
                    carol.empty_inbox()
                with Peer(server, tokens["carol"]) as carol:
                    carol.empty_inbox()
                with Console(client_executable, server) as bob:
                    bob.command(f"/register {prefix}-console example-password")
                    bob.expect("Registered user ")
                    bob.command(f"/login {credentials['bob']['username']} example-password")
                    bob.expect("Authenticated as user " + users["bob"] + ".")
                    bob.expect(f"[file {file_id}, message {file_message}]")
                    output = directory / "received notes.bin"
                    bob.command(f'/download {file_id} "{output}"')
                    bob.expect("Downloaded file " + file_id + " to ")
                    require(output.read_bytes() == content, "Console download changed binary content")
                    bob.command(f'/upload {users["alice"]} "{output}"')
                    bob.expect("Uploaded file ")
                    uploaded_by_console = alice.notice("file_available")
                    require(uploaded_by_console["size_bytes"] == len(content), "Console upload size changed")
                    alice.ack(uploaded_by_console["message_id"])
                    require(http(server, "GET", "/api/files/" + uploaded_by_console["file_id"],
                                 token=tokens["alice"]) == (200, content), "Console upload changed binary content")
                    bob.command(f"/send_group {group} Hello after restart")
                    bob.expect("to group " + group + ".")
                    reply = alice.notice("message")
                    require(reply["text"] == "Hello after restart" and reply["sender_id"] == users["bob"], "Console group reply failed")
                    alice.ack(reply["message_id"])
                    bob.command("/ping")
                    bob.expect("Pong.")
                    bob.command("/quit")
                    bob.expect("Disconnected.")
                    require(bob.process.wait(timeout=5) == 0, "Console failed to exit normally")
                with Peer(server, tokens["bob"]) as bob:
                    bob.empty_inbox()
                # Upload retries deliberately create distinct files; text retry keys do not apply.
                copies = []
                for _ in range(2):
                    status, saved = http(server, "POST", "/api/files", content, tokens["alice"], metadata)
                    require(status == 201, "Repeated upload failed")
                    copies.append(json.loads(saved)["file_id"])
                require(len({file_id, *copies}) == 3, "Unexpected file upload retry semantics")
            server.stop(signal.SIGTERM)
        print(f"PASS {shutdown_signal.name}: authentication, chat, groups, files, ACKs, retries, permissions, reconnect, and restart recovery")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--client-build-dir", type=Path, help="Use a separately built client")
    arguments = parser.parse_args()
    require(os.environ.get("CHATROOM_DB_NAME") == "chatroom_test", "Set CHATROOM_DB_NAME=chatroom_test; this script creates test records")
    require("CHATROOM_DB_PASSWORD" in os.environ, "Set CHATROOM_DB_PASSWORD before running this script")
    server = (arguments.build_dir / "server/chatroom_server").resolve()
    client = ((arguments.client_build_dir or arguments.build_dir) / "client/chatroom_client").resolve()
    require(server.is_file() and client.is_file(), "Build the server and client first")
    for shutdown_signal in (signal.SIGTERM, signal.SIGKILL):
        run_scenario(server, client, shutdown_signal)


if __name__ == "__main__":
    main()
