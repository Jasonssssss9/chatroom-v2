#!/usr/bin/env python3
"""Measure local TCP connections, persisted chat, and group fan-out with real ACKs."""

import argparse
import asyncio
import base64
from collections import Counter, OrderedDict
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import platform
import resource
import secrets
import signal
import subprocess
import tempfile
import time

from end_to_end import Server, require


def sql(statement):
    environment = dict(os.environ, MYSQL_PWD=os.environ["CHATROOM_DB_PASSWORD"])
    result = subprocess.run([
        "mysql", "--protocol=TCP", "--batch", "--skip-column-names",
        "--host=" + os.environ.get("CHATROOM_DB_HOST", "127.0.0.1"),
        "--port=" + os.environ.get("CHATROOM_DB_PORT", "3306"),
        "--user=" + os.environ.get("CHATROOM_DB_USER", "chatroom"), "chatroom_test"],
        input=statement, text=True, capture_output=True, env=environment, timeout=60)
    require(result.returncode == 0, "Benchmark SQL failed: " + result.stderr.strip())
    return [line.split("\t") for line in result.stdout.splitlines()]


class Fixture:
    def __init__(self, count):
        self.prefix = "bench-" + secrets.token_hex(6) + "-"
        values = ",".join(f"('{self.prefix}{index}','benchmark-only-no-password')" for index in range(count))
        sql("INSERT INTO users (username,password_hash) VALUES " + values)
        self.users = [row[0] for row in sql(f"SELECT id FROM users WHERE username LIKE '{self.prefix}%' ORDER BY id")]
        require(len(self.users) == count, "Benchmark user setup was incomplete")

    def clear_messages(self):
        # Delete only records owned by this run; preserve other test fixtures.
        condition = f"u.username LIKE '{self.prefix}%'"
        sql(f"DELETE d FROM message_deliveries d JOIN messages m ON m.id=d.message_id "
            f"JOIN users u ON u.id=m.sender_id WHERE {condition};"
            f"DELETE m FROM messages m JOIN users u ON u.id=m.sender_id WHERE {condition};"
            f"DELETE g FROM chat_groups g JOIN users u ON u.id=g.owner_id WHERE {condition}")

    def counts(self):
        return [int(row[0]) for row in sql(
            f"SELECT COUNT(*) FROM messages m JOIN users u ON u.id=m.sender_id WHERE u.username LIKE '{self.prefix}%';"
            f"SELECT COUNT(*) FROM message_deliveries d JOIN users u ON u.id=d.recipient_id WHERE u.username LIKE '{self.prefix}%';"
            f"SELECT COUNT(*) FROM message_deliveries d JOIN users u ON u.id=d.recipient_id "
            f"WHERE u.username LIKE '{self.prefix}%' AND d.acknowledged_at IS NULL")]

    def close(self):
        self.clear_messages()
        sql(f"DELETE FROM users WHERE username LIKE '{self.prefix}%'")


def token(secret, user):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).rstrip(b"=")
    now = int(time.time())
    payload = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode(
        {"iss": "chatroom-v2", "sub": user, "iat": now, "exp": now + 3600})
    signature = base64.urlsafe_b64encode(hmac.digest(secret.encode(), payload, "sha256")).rstrip(b"=")
    return (payload + b"." + signature).decode()


def process_snapshot(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    values = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)
    return {"cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
            "rss_mib": int(values["VmRSS"].split()[0]) / 1024,
            "threads": int(values["Threads"])}


def distribution(values):
    if not values:
        return {}
    ordered = sorted(values)
    return {name: round(ordered[max(0, math.ceil(len(ordered) * fraction) - 1)] * 1000, 3)
            for name, fraction in (("p50_ms", .50), ("p95_ms", .95), ("p99_ms", .99), ("max_ms", 1.0))}


class Measurements:
    def __init__(self, processes):
        self.processes = processes
        self.samples = {name: [] for name in processes}
        self.started = time.perf_counter()
        self.before = {name: process_snapshot(pid) for name, pid in processes.items()}
        self.task = asyncio.create_task(self.sample())

    async def sample(self):
        while True:
            for name, pid in self.processes.items():
                self.samples[name].append(process_snapshot(pid))
            await asyncio.sleep(.5)

    async def finish(self):
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)
        elapsed = time.perf_counter() - self.started
        results = {}
        for name, pid in self.processes.items():
            after = process_snapshot(pid)
            samples = self.samples[name] + [after]
            results[name] = {"cpu_percent": round(100 * (after["cpu_seconds"] - self.before[name]["cpu_seconds"]) / elapsed, 2),
                "rss_peak_mib": round(max(sample["rss_mib"] for sample in samples), 2),
                "rss_start_mib": round(self.before[name]["rss_mib"], 2),
                "rss_end_mib": round(after["rss_mib"], 2),
                "threads_peak": max(sample["threads"] for sample in samples)}
        return elapsed, results


class Peer:
    def __init__(self, reader, writer, user, driver):
        self.reader, self.writer, self.user, self.driver = reader, writer, user, driver
        self.pending = {}
        self.sequence = 0
        self.acks = set()
        self.receiving = asyncio.create_task(self.receive())

    async def request(self, action, data=None):
        self.sequence += 1
        request_id = str(self.sequence)
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        payload = json.dumps({"type": "request", "request_id": request_id, "action": action,
                              "data": data or {}}, separators=(",", ":")).encode()
        # write() queues the complete frame without yielding; all producers share this loop.
        self.writer.write(len(payload).to_bytes(4, "big") + payload)
        await self.writer.drain()
        try:
            response = await asyncio.wait_for(future, 30)
        finally:
            self.pending.pop(request_id, None)
        require(response.get("ok"), f"Request {action} failed: {response.get('error')}")
        return response["data"]

    async def receive(self):
        try:
            while True:
                size = int.from_bytes(await self.reader.readexactly(4), "big")
                require(size <= 65536, "Oversized response")
                message = json.loads(await self.reader.readexactly(size))
                if message["type"] == "response":
                    future = self.pending.get(message["request_id"])
                    if future and not future.done():
                        future.set_result(message)
                elif message.get("action") == "message":
                    job = asyncio.create_task(self.driver.delivery(self, message["data"], time.perf_counter()))
                    self.acks.add(job)
                    job.add_done_callback(self.acks.discard)
                elif message.get("action") != "group_created":
                    raise RuntimeError("Unexpected notification")
        except asyncio.CancelledError:
            pass
        except Exception as error:
            self.driver.errors.append(str(error))
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(error)

    async def close(self):
        self.receiving.cancel()
        for job in list(self.acks):
            job.cancel()
        await asyncio.gather(self.receiving, *self.acks, return_exceptions=True)
        self.writer.close()
        await self.writer.wait_closed()


class Driver:
    def __init__(self):
        self.peers = []
        self.operations = OrderedDict()
        self.errors = []
        self.latency = {"response": [], "delivery": [], "acknowledged": []}
        self.completed = self.deliveries = self.duplicates = self.total = 0
        self.sequence = 0

    async def connect(self, server, users, secret):
        semaphore = asyncio.Semaphore(64)
        async def connect_one(user):
            async with semaphore:
                reader, writer = await asyncio.open_connection("127.0.0.1", server.tcp_port)
                peer = Peer(reader, writer, user, self)
                self.peers.append(peer)
                await peer.request("authenticate", {"token": token(secret, user)})
                await peer.request("ping")  # Complete the empty authentication replay first.
        await asyncio.gather(*(connect_one(user) for user in users))
        self.peers.sort(key=lambda peer: int(peer.user))

    async def delivery(self, peer, data, observed):
        operation = None
        try:
            operation = self.operations[data["client_message_id"]]
            require(peer.user in operation["recipients"] and data["sender_id"] == operation["sender"], "Wrong delivery recipient or sender")
            require(data["text"] == operation["text"] and data.get("group_id") == operation["group"], "Delivery content changed")
            require(operation["message_id"] in (None, data["message_id"]), "Notification message ID changed")
            operation["message_id"] = data["message_id"]
            if peer.user in operation["seen"]:
                self.duplicates += 1
            else:
                operation["seen"].add(peer.user)
                if operation["record"]:
                    self.latency["delivery"].append(observed - operation["started"])
                    self.deliveries += 1
            response = await peer.request("ack_message", {"message_id": data["message_id"]})
            require(response["message_id"] == data["message_id"], "Wrong ACK response")
            operation["acked"].add(peer.user)
            if operation["acked"] == operation["recipients"]:
                operation["done"].set()
        except Exception as error:
            self.errors.append(str(error))
            if operation:
                operation["done"].set()

    async def message(self, peer, recipients, group, record):
        self.sequence += 1
        key = f"bench-{peer.user}-{self.sequence}"
        text = "m" * 128
        started = time.perf_counter()
        operation = {"started": started, "recipients": recipients, "sender": peer.user,
            "group": group, "text": text, "seen": set(), "acked": set(),
            "record": record, "done": asyncio.Event(), "message_id": None}
        self.operations[key] = operation
        data = {"client_message_id": key, "text": text}
        if group:
            data["group_id"] = group
        else:
            data["recipients"] = list(recipients)
        response = await peer.request("send_group_message" if group else "send_message", data)
        response_at = time.perf_counter()
        require(bool(response.get("message_id")), "Missing persisted message ID")
        require(operation["message_id"] in (None, response["message_id"]), "Response and notification IDs differ")
        operation["message_id"] = response["message_id"]
        await asyncio.wait_for(operation["done"].wait(), 30)
        require(operation["acked"] == recipients, "Incomplete message delivery")
        self.total += 1
        if record:
            self.completed += 1
            self.latency["response"].append(response_at - started)
            self.latency["acknowledged"].append(time.perf_counter() - started)
        # Keep a bounded history to recognize permitted duplicate notifications.
        while len(self.operations) > 2048:
            oldest = next(iter(self.operations))
            if not self.operations[oldest]["done"].is_set():
                break
            self.operations.popitem(last=False)

    async def traffic(self, kind, senders, duration, record=True):
        deadline = time.perf_counter() + duration
        async def produce(peer, recipients, group):
            while time.perf_counter() < deadline and not self.errors:
                if kind == "ping":
                    started = time.perf_counter()
                    await peer.request("ping")
                    if record:
                        self.completed += 1
                        self.latency["response"].append(time.perf_counter() - started)
                else:
                    await self.message(peer, recipients, group, record)
        tasks = [asyncio.create_task(produce(*sender)) for sender in senders]
        try:
            await asyncio.gather(*tasks)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    async def idle(self, duration, interval):
        deadline = time.perf_counter() + duration
        async def heartbeat(index, peer):
            await asyncio.sleep(interval * index / len(self.peers))
            while time.perf_counter() < deadline:
                started = time.perf_counter()
                await peer.request("ping")
                self.completed += 1
                self.latency["response"].append(time.perf_counter() - started)
                await asyncio.sleep(max(0, min(interval, deadline - time.perf_counter())))
        await asyncio.gather(*(heartbeat(index, peer) for index, peer in enumerate(self.peers)))
        # Check every connection once more after the hold, outside the measurement window.

    async def close(self):
        await asyncio.gather(*(peer.close() for peer in self.peers), return_exceptions=True)


def database_status():
    names = "'Com_commit','Com_stmt_execute','Innodb_log_waits','Innodb_row_lock_waits','Innodb_row_lock_time','Innodb_os_log_fsyncs'"
    return {row[0]: int(row[1]) for row in sql("SHOW GLOBAL STATUS WHERE Variable_name IN (" + names + ")")}


async def run_case(arguments, fixture, case, index, mysql_pid):
    kind, count, workers, duration = case
    name = f"{kind}-{count}-w{workers}-r{index}"
    print("START " + name, flush=True)
    fixture.clear_messages()
    driver = Driver()
    secret = secrets.token_hex(32)
    log_path = arguments.output / (name + ".jsonl")
    with tempfile.TemporaryDirectory(prefix="chatroom-benchmark-") as temporary:
        config = Path(temporary) / "config.json"
        config.write_text(json.dumps({"server": {"port": 0, "worker_threads": workers,
            "max_connections": max(1024, count), "idle_timeout_seconds": 90}, "http": {"port": 0},
            "files": {"directory": temporary + "/files"}, "log": {"level": "info"}}))
        with Server(arguments.server, config, secret, log_path) as server:
            result = {"name": name, "kind": kind, "connections": count, "workers": workers,
                      "requested_seconds": duration, "repeat": index,
                      "server_baseline": process_snapshot(server.process.pid)}
            measurement = None
            try:
                started = time.perf_counter()
                await driver.connect(server, fixture.users[:count], secret)
                result["setup_seconds"] = round(time.perf_counter() - started, 3)
                result["server_connected"] = process_snapshot(server.process.pid)
                result["server_fd_count"] = len(list(Path(f"/proc/{server.process.pid}/fd").iterdir()))
                if kind == "group":
                    senders = []
                    for offset in range(0, count, 64):
                        members = driver.peers[offset:offset + 64]
                        created = await members[0].request("create_group", {"name": "Benchmark group",
                            "members": [peer.user for peer in members[1:]]})
                        senders.append((members[0], set(peer.user for peer in members[1:]), created["group_id"]))
                else:
                    senders = [(peer, {driver.peers[position ^ 1].user}, None)
                               for position, peer in enumerate(driver.peers)]
                if kind != "idle":
                    await driver.traffic(kind, senders, arguments.warmup, record=False)
                before = database_status()
                processes = {"server": server.process.pid, "generator": os.getpid()}
                if mysql_pid:
                    processes["mysql"] = mysql_pid
                measurement = Measurements(processes)
                if kind == "idle":
                    await driver.idle(duration, min(arguments.heartbeat, duration / 2))
                else:
                    await driver.traffic(kind, senders, duration)
                elapsed, resources = await measurement.finish()
                measurement = None
                after = database_status()
                result.update({"seconds": round(elapsed, 3), "operations": driver.completed,
                    "operations_per_second": round(driver.completed / elapsed, 2),
                    "deliveries": driver.deliveries, "deliveries_per_second": round(driver.deliveries / elapsed, 2),
                    "duplicates": driver.duplicates, "latency": {key: distribution(value) for key, value in driver.latency.items()},
                    "resources": resources, "mysql_status_delta": {key: after[key] - value for key, value in before.items()}})
                if kind == "idle":
                    semaphore = asyncio.Semaphore(64)
                    async def verify(peer):
                        async with semaphore:
                            await peer.request("ping")
                    await asyncio.gather(*(verify(peer) for peer in driver.peers))
                    result["connections_verified_after_hold"] = len(driver.peers)
                elif kind in ("private", "group", "soak"):
                    counts = fixture.counts()
                    expected = [driver.total, driver.total * (63 if kind == "group" else 1), 0]
                    require(counts == expected, f"Persistence mismatch: {counts}, expected {expected}")
                    result["persisted_messages"], result["persisted_deliveries"], result["unacknowledged"] = counts
                require(not driver.errors, "Peer errors: " + str(driver.errors[:3]))
                result["errors"] = []
            except Exception as error:
                result["errors"] = [str(error)] + driver.errors[:10]
            finally:
                if measurement:
                    measurement.task.cancel()
                    await asyncio.gather(measurement.task, return_exceptions=True)
                await driver.close()
                server.stop(signal.SIGTERM)
            counts = Counter()
            for line in log_path.read_text().splitlines():
                entry = json.loads(line)
                if entry["level"] in ("warn", "error"):
                    counts[entry["event"]] += 1
            result["server_warnings_errors"] = dict(counts)
            if counts and not result["errors"]:
                result["errors"].append("Server reported warnings or errors")
    print(json.dumps(result), flush=True)
    return result


def environment():
    cpu = next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name"))
    mysql_fields = ("version", "innodb_flush_log_at_trx_commit", "sync_binlog", "innodb_buffer_pool_size", "log_bin", "max_connections")
    mysql_values = sql("SELECT @@version,@@innodb_flush_log_at_trx_commit,@@sync_binlog,@@innodb_buffer_pool_size,@@log_bin,@@max_connections")[0]
    return {"platform": platform.platform(), "cpu": cpu, "logical_cpus": os.cpu_count(),
        "memory_kib": int(Path("/proc/meminfo").read_text().splitlines()[0].split()[1]),
        "open_file_limit": resource.getrlimit(resource.RLIMIT_NOFILE)[0], "python": platform.python_version(),
        "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "mysql": dict(zip(mysql_fields, mysql_values))}


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float, default=20)
    parser.add_argument("--warmup", type=float, default=3)
    parser.add_argument("--idle-duration", type=float, default=45)
    parser.add_argument("--heartbeat", type=float, default=15)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--pilot", action="store_true", help="Run short checks before the full matrix")
    parser.add_argument("--case", nargs=4, action="append", metavar=("KIND", "CONNECTIONS", "WORKERS", "SECONDS"),
                        help="Run a selected case instead of the default matrix; may be repeated")
    arguments = parser.parse_args()
    require(os.environ.get("CHATROOM_DB_NAME") == "chatroom_test", "Use only CHATROOM_DB_NAME=chatroom_test")
    require("CHATROOM_DB_PASSWORD" in os.environ, "Set CHATROOM_DB_PASSWORD")
    require(arguments.duration > 0 and arguments.idle_duration > 0 and arguments.heartbeat > 0 and
            arguments.warmup >= 0 and arguments.repeats > 0, "Durations and repeats must be positive; warmup may be zero")
    arguments.server = arguments.server.resolve()
    arguments.output.mkdir(parents=True, exist_ok=True)
    mysql = subprocess.run(["pgrep", "-x", "mysqld"], capture_output=True, text=True)
    mysql_pid = int(mysql.stdout.split()[0]) if mysql.stdout.strip() else None
    report = {"started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "environment": environment(), "server_sha256": hashlib.sha256(arguments.server.read_bytes()).hexdigest(),
              "settings": {"warmup_seconds": arguments.warmup, "duration_seconds": arguments.duration,
                  "idle_seconds": arguments.idle_duration, "heartbeat_seconds": arguments.heartbeat,
                  "body_bytes": 128, "group_members": 64, "max_pending_requests": 1024,
                  "max_requests_per_connection": 32, "ack_required": True, "model": "closed loop"}, "cases": []}
    if arguments.case:
        cases = []
        for kind, count, workers, seconds in arguments.case:
            count, workers, seconds = int(count), int(workers), float(seconds)
            require(kind in ("idle", "ping", "private", "group", "soak") and count >= 2 and count % 2 == 0 and
                    workers > 0 and seconds > 0 and (kind != "group" or count % 64 == 0), "Invalid selected case")
            cases.append(((kind, count, workers, seconds), 1))
    else:
        if arguments.pilot:
            cases = [(("idle", 128, 4, 3), 1), (("ping", 32, 4, 2), 1),
                     (("private", 32, 4, 2), 1), (("group", 64, 4, 2), 1)]
        else:
            cases = [(("idle", count, 4, arguments.idle_duration), 1) for count in (1000, 5000, 10000)]
            for repeat in range(1, arguments.repeats + 1):
                for kind, count, workers in (("ping", 32, 4), ("ping", 256, 4),
                        ("private", 16, 4), ("private", 64, 4), ("private", 256, 4),
                        ("private", 256, 8), ("group", 64, 4), ("group", 256, 4), ("group", 512, 4)):
                    cases.append(((kind, count, workers, arguments.duration), repeat))
            cases.append((("soak", 64, 4, 120), 1))
    fixture = Fixture(max(case[0][1] for case in cases))
    try:
        for case, repeat in cases:
            report["cases"].append(await run_case(arguments, fixture, case, repeat, mysql_pid))
            (arguments.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    finally:
        fixture.close()
    require(all(not case["errors"] for case in report["cases"]), "Some benchmark cases failed; inspect results.json")


if __name__ == "__main__":
    asyncio.run(main())
