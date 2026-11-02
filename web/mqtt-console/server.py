#!/usr/bin/env python3
"""Local graphical console for the Shunya gateway MQTT API.

MQTT credentials are read from src/gateway/config.h. They stay in this process
and are never sent to the browser or written to logs.
"""

from __future__ import annotations

import argparse
import json
import queue
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "src" / "gateway" / "config.h"
STATIC = Path(__file__).resolve().parent / "static"
PREFIX_RE = re.compile(r"^[0-9A-Fa-f]{12}$")
MAX_BODY = 8192


def read_define(name: str) -> str:
    text = CONFIG.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(?:\"([^\"]*)\"|([^\s/]+))",
        text,
        re.MULTILINE,
    )
    if not match:
        raise RuntimeError(f"Missing {name} in {CONFIG}")
    return match.group(1) if match.group(1) is not None else match.group(2)


class Broker:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.clients: list[queue.Queue[dict[str, Any]]] = []
        self.devices: dict[str, dict[str, Any]] = {}
        self.connected = threading.Event()
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"shunya-console-{int(time.time()) & 0xFFFFFF:06x}",
        )
        self.client.username_pw_set(read_define("MQTT_USER"), read_define("MQTT_PASSWORD"))
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.connect(read_define("MQTT_SERVER"), int(read_define("MQTT_PORT")), 30)
        self.client.loop_start()
        if not self.connected.wait(10):
            raise TimeoutError("MQTT connection timed out")

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any,
                    reason_code: Any, properties: Any) -> None:
        if reason_code.is_failure:
            return
        client.subscribe([
            ("+/status", 0),
            ("+/event_log", 0),
            ("+/node/+/data", 0),
        ])
        self.connected.set()
        self.broadcast({"type": "broker", "connected": True})

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, flags: Any,
                       reason_code: Any, properties: Any) -> None:
        self.connected.clear()
        self.broadcast({"type": "broker", "connected": False})

    def _on_message(self, client: mqtt.Client, userdata: Any,
                    message: mqtt.MQTTMessage) -> None:
        parts = message.topic.split("/")
        if not parts or not PREFIX_RE.fullmatch(parts[0]):
            return
        prefix = parts[0].upper()
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(payload, dict):
            return
        event: dict[str, Any] | None = None
        with self.lock:
            device = self.devices.setdefault(prefix, {"prefix": prefix})
            if message.topic.endswith("/status") and "firmware_version" in payload:
                device["status"] = payload
                device["seen_at"] = time.time()
                event = {"type": "status", "prefix": prefix, "payload": payload}
            elif message.topic.endswith("/status") and "link_test" in payload:
                event = {"type": "link_test", "prefix": prefix, "payload": payload}
            elif message.topic.endswith("/event_log"):
                event = {"type": "event_log", "prefix": prefix, "payload": payload}
            elif len(parts) == 4 and parts[1] == "node" and parts[3] == "data":
                event = {"type": "node", "prefix": prefix, "payload": payload}
        if event:
            self.broadcast(event)

    def broadcast(self, event: dict[str, Any]) -> None:
        with self.lock:
            clients = list(self.clients)
        for client in clients:
            try:
                client.put_nowait(event)
            except queue.Full:
                pass

    def subscribe_events(self) -> queue.Queue[dict[str, Any]]:
        events: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=200)
        with self.lock:
            self.clients.append(events)
            snapshot = {
                "type": "snapshot",
                "broker": self.connected.is_set(),
                "devices": list(self.devices.values()),
            }
        events.put(snapshot)
        return events

    def unsubscribe(self, events: queue.Queue[dict[str, Any]]) -> None:
        with self.lock:
            if events in self.clients:
                self.clients.remove(events)

    def publish(self, prefix: str, payload: dict[str, Any]) -> None:
        if not PREFIX_RE.fullmatch(prefix):
            raise ValueError("device prefix must be a 12-digit MAC")
        body = json.dumps(payload, separators=(",", ":"))
        if len(body.encode("utf-8")) > MAX_BODY:
            raise ValueError("command is larger than 8 KB")
        info = self.client.publish(f"{prefix.upper()}/command", body)
        info.wait_for_publish(5)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"MQTT publish failed ({info.rc})")

    def device_list(self) -> list[dict[str, Any]]:
        with self.lock:
            return list(self.devices.values())

    def close(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()


class ConsoleHandler(BaseHTTPRequestHandler):
    broker: Broker

    def log_message(self, format: str, *args: Any) -> None:
        print(f"[console] {self.address_string()} {format % args}")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/stream":
            self._stream()
            return
        if path == "/api/devices":
            self._json(200, {"devices": self.broker.device_list()})
            return
        if path == "/":
            path = "/index.html"
        self._static(path)

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/api/command":
            self._json(404, {"error": "not found"})
            return
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY:
            self._json(400, {"error": "invalid command size"})
            return
        try:
            body = json.loads(self.rfile.read(length))
            prefix = str(body["prefix"])
            payload = body["payload"]
            if not isinstance(payload, dict):
                raise ValueError("payload must be a JSON object")
            self.broker.publish(prefix, payload)
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            self._json(400, {"error": str(error)})
            return
        except Exception as error:
            self._json(502, {"error": str(error)})
            return
        self._json(200, {"ok": True})

    def _stream(self) -> None:
        events = self.broker.subscribe_events()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            while True:
                try:
                    event = events.get(timeout=15)
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    continue
                data = json.dumps(event, separators=(",", ":")).encode("utf-8")
                self.wfile.write(b"data: " + data + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            self.broker.unsubscribe(events)

    def _static(self, path: str) -> None:
        relative = path.lstrip("/")
        target = (STATIC / relative).resolve()
        if STATIC not in target.parents and target != STATIC:
            self._json(403, {"error": "forbidden"})
            return
        if not target.is_file():
            self._json(404, {"error": "not found"})
            return
        kind = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".js": "text/javascript; charset=utf-8",
        }.get(target.suffix, "application/octet-stream")
        data = target.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, status: int, payload: dict[str, Any]) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Shunya gateway MQTT console")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    broker = Broker()
    ConsoleHandler.broker = broker
    server = ThreadingHTTPServer((args.host, args.port), ConsoleHandler)
    print(f"Shunya MQTT console: http://{args.host}:{args.port}", flush=True)
    print("Credentials stay on this process. Close with Ctrl+C.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        server.server_close()
        broker.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
