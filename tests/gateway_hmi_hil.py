#!/usr/bin/env python3
"""Gateway + attached-HMI hardware-in-the-loop test runner.

The runner deliberately reads MQTT credentials from src/gateway/config.h so
they are never copied into test artifacts or printed. It exercises only
recoverable behavior and always attempts to leave Manual mode, pairing closed,
diagnostics disabled, and every relay off.
"""

from __future__ import annotations

import argparse
import json
import queue
import random
import re
import threading
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

import paho.mqtt.client as mqtt
import serial


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "src" / "gateway" / "config.h"


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


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


RELAY_COUNT = int(read_define("RELAY_COUNT"))


def relay_vector(*indexes: int) -> list[bool]:
    state = [False] * RELAY_COUNT
    for index in indexes:
        state[index] = True
    return state


@dataclass
class Result:
    name: str
    status: str
    detail: str
    started_at: str
    duration_ms: int


class Recorder:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.output.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.stem = self.output / f"gateway-hmi-{stamp}"
        self.results: list[Result] = []
        self.serial_log = self.stem.with_suffix(".serial.log").open(
            "w", encoding="utf-8", buffering=1
        )
        self.mqtt_log = self.stem.with_suffix(".mqtt.jsonl").open(
            "w", encoding="utf-8", buffering=1
        )

    def add(self, name: str, status: str, detail: str, start: float) -> None:
        result = Result(
            name, status, detail, utc_now(), int((time.monotonic() - start) * 1000)
        )
        self.results.append(result)
        print(f"[{status:7}] {name}: {detail}")

    def close(self, metadata: dict[str, Any]) -> Path:
        self.serial_log.close()
        self.mqtt_log.close()
        report = self.stem.with_suffix(".json")
        report.write_text(
            json.dumps(
                {
                    "generated_at": utc_now(),
                    "metadata": metadata,
                    "summary": {
                        state: sum(r.status == state for r in self.results)
                        for state in ("PASS", "FAIL", "BLOCKED", "SKIP")
                    },
                    "results": [asdict(result) for result in self.results],
                },
                indent=2,
            ),
            encoding="utf-8",
        )
        return report


class SerialLink:
    def __init__(self, port: str, recorder: Recorder) -> None:
        self.recorder = recorder
        self.port = serial.Serial(port, 115200, timeout=0.1)
        self.lines: queue.Queue[str] = queue.Queue()
        self.history: list[str] = []
        self.running = True
        self.reset_count = 0
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self) -> None:
        while self.running:
            raw = self.port.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            self.history.append(line)
            self.recorder.serial_log.write(f"{utc_now()} {line}\n")
            self.lines.put(line)

    def send(self, command: str) -> None:
        self.port.write((command + "\n").encode("ascii"))
        self.port.flush()

    def reset(self) -> None:
        if self.reset_count:
            # Native USB CDC reliably resets on the first control-line edge
            # after opening. Reopen for later persistence-reset phases.
            self.running = False
            self.thread.join(timeout=1)
            self.port.close()
            time.sleep(0.3)
            self.port = serial.Serial(self.port.port, 115200, timeout=0.1)
            self.running = True
            self.thread = threading.Thread(target=self._reader, daemon=True)
            self.thread.start()
        # Force an RTS edge while DTR is inactive; this works both immediately
        # after opening the port and for later persistence reboots.
        self.port.dtr = False
        self.port.rts = False
        time.sleep(0.1)
        self.port.rts = True
        time.sleep(0.25)
        self.port.rts = False
        self.reset_count += 1

    def wait_line(self, predicate: Callable[[str], bool], timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=min(0.2, deadline - time.monotonic()))
            except queue.Empty:
                continue
            if predicate(line):
                return line
        raise TimeoutError("serial evidence timeout")

    def collect(self, seconds: float) -> list[str]:
        start = len(self.history)
        time.sleep(seconds)
        return self.history[start:]

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1)
        self.port.close()


class MqttLink:
    def __init__(self, prefix: str, recorder: Recorder) -> None:
        self.prefix = prefix
        self.recorder = recorder
        self.command_topic = f"{prefix}/command"
        self.status_topic = f"{prefix}/status"
        self.event_topic = f"{prefix}/event_log"
        self.statuses: list[dict[str, Any]] = []
        self.link_results: list[dict[str, Any]] = []
        self.events: list[dict[str, Any]] = []
        self.condition = threading.Condition()
        self.connected = threading.Event()
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"shunya-hil-{random.randrange(1 << 24):06x}",
        )
        self.client.username_pw_set(read_define("MQTT_USER"), read_define("MQTT_PASSWORD"))
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect(read_define("MQTT_SERVER"), int(read_define("MQTT_PORT")), 30)
        self.client.loop_start()
        if not self.connected.wait(10):
            raise TimeoutError("MQTT connection timeout")

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any,
                    reason_code: Any, properties: Any) -> None:
        if not reason_code.is_failure:
            client.subscribe([(self.status_topic, 0), (self.event_topic, 0)])
            self.connected.set()

    def _on_message(self, client: mqtt.Client, userdata: Any,
                    message: mqtt.MQTTMessage) -> None:
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            payload = {"raw_bytes": len(message.payload)}
        self.recorder.mqtt_log.write(
            json.dumps({"at": utc_now(), "topic": message.topic, "payload": payload})
            + "\n"
        )
        with self.condition:
            if message.topic == self.status_topic:
                if isinstance(payload, dict) and "link_test" in payload:
                    self.link_results.append(payload)
                elif isinstance(payload, dict) and "firmware_version" in payload:
                    self.statuses.append(payload)
            elif isinstance(payload, dict):
                self.events.append(payload)
            self.condition.notify_all()

    def publish(self, payload: dict[str, Any] | str) -> None:
        body = payload if isinstance(payload, str) else json.dumps(payload)
        info = self.client.publish(self.command_topic, body)
        info.wait_for_publish(5)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"MQTT publish failed: {info.rc}")

    def wait_status(
        self, predicate: Callable[[dict[str, Any]], bool], timeout: float = 8,
        start_at: int | None = None,
    ) -> dict[str, Any]:
        start = len(self.statuses) if start_at is None else start_at
        deadline = time.monotonic() + timeout
        with self.condition:
            while time.monotonic() < deadline:
                for status in self.statuses[start:]:
                    if predicate(status):
                        return status
                self.condition.wait(timeout=min(0.25, deadline - time.monotonic()))
        raise TimeoutError("matching MQTT status timeout")

    def latest(self) -> dict[str, Any]:
        if not self.statuses:
            self.wait_status(lambda _: True, 12)
        return self.statuses[-1]

    def await_fresh_status(self) -> dict[str, Any]:
        # A retained status can arrive before the gateway has re-subscribed
        # after reset. Exclude early retained delivery and wait for the next
        # live 10-second publication before sending commands.
        time.sleep(1)
        start = len(self.statuses)
        return self.wait_status(lambda status: status.get("mqtt_connected") is True,
                                15, start)

    def close(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()


class HilRunner:
    def __init__(self, serial_link: SerialLink, mqtt_link: MqttLink,
                 recorder: Recorder) -> None:
        self.serial = serial_link
        self.mqtt = mqtt_link
        self.recorder = recorder
        self.display_error_baseline = (0, 0)

    def case(self, name: str, action: Callable[[], str]) -> None:
        start = time.monotonic()
        try:
            detail = action()
            self.recorder.add(name, "PASS", detail, start)
        except Exception as error:  # continue to cleanup and report all failures
            self.recorder.add(name, "FAIL", str(error), start)

    def command_status(
        self, payload: dict[str, Any] | str,
        predicate: Callable[[dict[str, Any]], bool],
        timeout: float = 8,
    ) -> dict[str, Any]:
        start = len(self.mqtt.statuses)
        self.mqtt.publish(payload)
        return self.mqtt.wait_status(predicate, timeout, start)

    def safe_state(self) -> None:
        self.command_status({"mode": 0}, lambda s: s.get("mode") == 0)
        self.command_status({"all": False}, lambda s: s.get("relays") == [False] * RELAY_COUNT)
        self.command_status({"pair": False}, lambda s: s.get("pairing_mode") is False)
        self.serial.send("diagnosticApp off")

    def final_check(self) -> str:
        self.safe_state()
        self.serial.send("stats")
        stats = self.serial.wait_line(
            lambda line: "online=" in line and "crc_err=" in line, 10
        )
        if "online=1" not in stats:
            raise AssertionError(stats)
        self.serial.send("test display")
        echo = self.serial.wait_line(
            lambda line: line.startswith("DISPLAY: echo="), 5
        )
        if "echo=ok" not in echo:
            raise AssertionError(echo)
        status = self.mqtt.latest()
        if (
            status.get("mode") != 0
            or status.get("relays") != [False] * RELAY_COUNT
            or status.get("pairing_mode") is not False
        ):
            raise AssertionError("unsafe final MQTT state")
        return f"Manual/all OFF/pairing closed; {stats.strip()}; {echo}"

    def classify_hub_unavailable(self) -> None:
        started = time.monotonic()
        self.serial.send("test hub")
        line = self.serial.wait_line(
            lambda item: item.startswith("HUB: status "), 5
        )
        if "FAIL" in line:
            self.recorder.add(
                "HUB hardware-dependent link", "BLOCKED",
                "expected: no ESP32-C6 hub attached", started,
            )
        else:
            self.recorder.add(
                "HUB hardware-dependent link", "PASS", line, started,
            )

    def baseline(self) -> None:
        def boot() -> str:
            recent = "\n".join(self.serial.history)
            if "Firmware Version:" not in recent or "Tasks started." not in recent:
                raise AssertionError("boot banner/task startup not observed")
            status = self.mqtt.latest()
            if not status.get("wifi_connected") or not status.get("mqtt_connected"):
                raise AssertionError("Wi-Fi or MQTT is offline")
            return f"firmware={status.get('firmware_version')} ip={status.get('ip_address')}"

        self.case("BOOT-001 gateway boot/network", boot)

        def display_stats() -> str:
            self.serial.send("stats")
            line = self.serial.wait_line(lambda x: "online=" in x and "crc_err=" in x, 10)
            values = dict(re.findall(r"(\w+)=([0-9]+)", line))
            if values.get("online") != "1":
                raise AssertionError(line)
            self.display_error_baseline = (
                int(values.get("crc_err", "-1")),
                int(values.get("framing_err", "-1")),
            )
            return line.strip()

        self.case("HMI-UART baseline stats", display_stats)

        def echo_soak() -> str:
            rtts = []
            for _ in range(10):
                self.serial.send("test display")
                line = self.serial.wait_line(lambda x: x.startswith("DISPLAY: echo="), 3)
                match = re.search(r"echo=ok rtt=(\d+)ms", line)
                if not match:
                    raise AssertionError(line)
                rtts.append(int(match.group(1)))
            self.serial.send("stats")
            stats = self.serial.wait_line(
                lambda x: "online=" in x and "crc_err=" in x, 5
            )
            values = dict(re.findall(r"(\w+)=([0-9]+)", stats))
            errors = (int(values["crc_err"]), int(values["framing_err"]))
            if errors != self.display_error_baseline:
                raise AssertionError(
                    f"UART errors grew {self.display_error_baseline}->{errors}"
                )
            return (
                f"10/10 echoes, min={min(rtts)}ms max={max(rtts)}ms; "
                f"UART errors stable at crc/framing={errors[0]}/{errors[1]}"
            )

        self.case("HMI-UART 10x echo soak", echo_soak)

    def gateway_cases(self) -> None:
        self.case(
            "MAN-003 initialize all off",
            lambda: (
                self.command_status({"mode": 0}, lambda s: s.get("mode") == 0),
                self.command_status({"all": False}, lambda s: s.get("relays") == [False] * RELAY_COUNT),
                "manual mode, all relays off",
            )[-1],
        )

        for index in range(RELAY_COUNT):
            def relay_case(i: int = index) -> str:
                desired = [False] * RELAY_COUNT
                desired[i] = True
                self.command_status(
                    {f"relay{i + 1}": True},
                    lambda s, expected=desired: s.get("relays") == expected,
                )
                self.command_status({"all": False}, lambda s: s.get("relays") == [False] * RELAY_COUNT)
                return f"relay {i + 1} ON verified then all OFF"
            self.case(f"MAN relay {index + 1}", relay_case)

        self.case(
            "MAN-004 mixed relay mask",
            lambda: (
                self.command_status(
                    {"relay1": True, "relay2": False, "relay3": True, "relay4": True},
                    lambda s: s.get("relays") == relay_vector(0, 2, 3),
                ),
                self.command_status({"all": False}, lambda s: s.get("relays") == [False] * RELAY_COUNT),
                "mask 0xD verified",
            )[-1],
        )

        def mode_protection() -> str:
            self.command_status({"mode": 1}, lambda s: s.get("mode") == 1)
            before = len(self.mqtt.statuses)
            self.mqtt.publish({"all": True})
            time.sleep(2)
            statuses = self.mqtt.statuses[before:]
            if any(any(s.get("relays", [])) for s in statuses):
                raise AssertionError("manual relay command changed Automation outputs")
            return "manual relay JSON ignored in Automation mode"
        self.case("MAN-005 mode protection", mode_protection)

        def automation_config() -> str:
            status = self.command_status(
                {"automation": {"rules": [
                    {"node": 1, "relay": 1, "temp": {"min": 20, "max": 25}},
                    {"node": 2, "relay": 2, "humidity": {"min": 40, "max": 60}},
                ]}},
                lambda s: s.get("automation_rule_count") == 2,
            )
            return f"{status['automation_rule_count']} rules stored; runtime blocked without nodes"
        self.case("AUT configuration storage", automation_config)

        def hybrid_config() -> str:
            self.command_status({"mode": 3}, lambda s: s.get("mode") == 3)
            status = self.command_status(
                {"hybrid": {"logic": "AND"}},
                lambda s: s.get("hybrid", {}).get("logic") == "AND",
            )
            return f"logic={status['hybrid']['logic']}; runtime blocked without nodes"
        self.case("HYB configuration storage", hybrid_config)

        self.case(
            "PAIR local window",
            lambda: (
                self.command_status({"pair": True}, lambda s: s.get("pairing_mode") is True),
                self.command_status({"pair": False}, lambda s: s.get("pairing_mode") is False),
                "pairing opened and closed; joining blocked without hub",
            )[-1],
        )

        for value, expected in ((1, 5), (4001, 3600), (37, 37)):
            self.case(
                f"SLEEP clamp {value}->{expected}",
                lambda value=value, expected=expected: (
                    self.command_status(
                        {"sensor_sleep": value},
                        lambda s, expected=expected: s.get("sensor_sleep_sec") == expected,
                    ),
                    f"sensor_sleep_sec={expected}",
                )[-1],
            )

        def queue_sequence() -> str:
            self.command_status({"mode": 2}, lambda s: s.get("mode") == 2)
            config = {"queue": {"loop": False, "auto_start": True, "steps": [
                {"relay": 1, "duration": 2},
                {"delay": 2},
                {"relays": [2, 3], "duration": 2},
            ]}}
            observed: list[tuple[int, list[bool]]] = []
            start = len(self.mqtt.statuses)
            self.mqtt.publish(config)
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                for state in self.mqtt.statuses[start:]:
                    queue_state = state.get("queue", {})
                    item = (queue_state.get("current_step", 0), state.get("relays", []))
                    if item not in observed:
                        observed.append(item)
                if observed and observed[-1] == (0, [False] * RELAY_COUNT):
                    break
                time.sleep(0.2)
            required = [
                (1, relay_vector(0)),
                (2, [False] * RELAY_COUNT),
                (3, relay_vector(1, 2)),
                (0, [False] * RELAY_COUNT),
            ]
            if any(item not in observed for item in required):
                raise AssertionError(f"missing queue states; observed={observed}")
            return f"observed {observed}"
        self.case("QUE sequence/delay/non-loop", queue_sequence)

        def queue_controls() -> str:
            self.mqtt.publish({"queue": {"loop": True, "auto_start": True, "steps": [
                {"relay": 1, "duration": 5}, {"relay": 2, "duration": 5}
            ]}})
            self.mqtt.wait_status(
                lambda s: s.get("queue", {}).get("running") is True, 8
            )
            self.command_status(
                {"queue_control": "pause"},
                lambda s: s.get("queue", {}).get("paused") is True,
            )
            time.sleep(2)
            self.command_status(
                {"queue_control": "resume"},
                lambda s: s.get("queue", {}).get("paused") is False,
            )
            self.command_status(
                {"queue_control": "restart"},
                lambda s: s.get("queue", {}).get("current_step") == 1,
            )
            self.command_status(
                {"queue_control": "stop"},
                lambda s: s.get("queue", {}).get("running") is False
                and s.get("relays") == [False] * RELAY_COUNT,
            )
            return "pause/resume/restart/stop verified"
        self.case("QUE controls", queue_controls)

        def malformed() -> str:
            self.mqtt.publish('{"relay1":')
            line = self.serial.wait_line(lambda x: "JSON parse failed" in x, 5)
            self.mqtt.publish("{" + '"x":"' + ("A" * 4000) + '"}')
            line2 = self.serial.wait_line(lambda x: "JSON parse failed" in x, 5)
            return f"malformed and oversized rejected ({line}; {line2})"
        self.case("MQTT malformed/oversized robustness", malformed)

        def burst() -> str:
            self.command_status({"mode": 0}, lambda s: s.get("mode") == 0)
            for i in range(20):
                self.mqtt.publish({f"relay{(i % RELAY_COUNT) + 1}": bool(i & 1)})
            self.mqtt.publish({"all": False})
            self.mqtt.wait_status(lambda s: s.get("relays") == [False] * RELAY_COUNT, 12)
            time.sleep(2)
            return "21-command burst survived; final all-off status received"
        self.case("MQTT command burst", burst)

        def event_log() -> str:
            self.mqtt.publish({"event_log": "clear"})
            time.sleep(0.5)
            start = len(self.mqtt.events)
            self.mqtt.publish({"event_log": "count"})
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                for item in self.mqtt.events[start:]:
                    if item.get("status") == "count":
                        if item.get("count") != 0:
                            raise AssertionError(f"event log count={item.get('count')}")
                        return "event log cleared and count=0"
                time.sleep(0.1)
            raise TimeoutError("event log count response timeout")
        self.case("LOG clear/count", event_log)

    def queue_boundaries(self) -> None:
        self.command_status({"mode": 2}, lambda s: s.get("mode") == 2)
        self.case(
            "QUE empty input",
            lambda: (
                self.command_status(
                    {"queue": {"loop": False, "auto_start": False, "steps": []}},
                    lambda s: s.get("queue", {}).get("total_steps") == 0
                    and s.get("queue", {}).get("running") is False,
                ),
                "empty queue accepted and idle",
            )[-1],
        )

        def sized_queue(count: int, expected: int) -> str:
            steps = [{"delay": 1} for _ in range(count)]
            status = self.command_status(
                {"queue": {"loop": False, "auto_start": True, "steps": steps}},
                lambda s: s.get("queue", {}).get("total_steps") == expected,
            )
            self.command_status(
                {"queue_control": "stop"},
                lambda s: s.get("queue", {}).get("running") is False,
            )
            return f"{count} supplied, {status['queue']['total_steps']} stored"

        self.case("QUE maximum 20 steps", lambda: sized_queue(20, 20))
        self.case("QUE overflow truncation", lambda: sized_queue(25, 20))

        def immediate_exit() -> str:
            self.mqtt.publish({"queue": {"loop": True, "auto_start": True, "steps": [
                {"relay": 1, "duration": 10}
            ]}})
            self.mqtt.wait_status(
                lambda s: s.get("relays") == relay_vector(0), 8
            )
            status = self.command_status(
                {"mode": 0}, lambda s: s.get("mode") == 0
                and s.get("relays") == [False] * RELAY_COUNT,
            )
            return f"immediate exit safe at uptime {status['uptime_ms']} ms"
        self.case("QUE immediate mode exit", immediate_exit)

        def graceful_exit() -> str:
            self.command_status({"mode": 2}, lambda s: s.get("mode") == 2)
            self.mqtt.publish({"queue": {"loop": True, "auto_start": True, "steps": [
                {"relay": 2, "duration": 3}, {"delay": 1}
            ]}})
            self.mqtt.wait_status(
                lambda s: s.get("relays") == relay_vector(1), 8
            )
            started = time.monotonic()
            first = self.command_status(
                {"mode": 0, "transition": "graceful"},
                lambda s: s.get("mode") == 0
                or (
                    s.get("mode") == 2
                    and s.get("queue", {}).get("loop_enabled") is False
                ),
            )
            if first.get("mode") != 2:
                raise AssertionError("graceful exit did not defer mode transition")
            self.mqtt.wait_status(
                lambda s: s.get("mode") == 0 and s.get("relays") == [False] * RELAY_COUNT,
                12,
            )
            elapsed = time.monotonic() - started
            if elapsed < 2:
                raise AssertionError(f"graceful exit completed too early ({elapsed:.2f}s)")
            return f"deferred transition completed safely in {elapsed:.2f}s"
        self.case("QUE graceful mode exit", graceful_exit)

    def persistence_reboot(self) -> None:
        def run() -> str:
            self.command_status({"mode": 2}, lambda s: s.get("mode") == 2)
            self.command_status(
                {"queue": {"loop": False, "auto_start": False, "steps": [
                    {"relay": 4, "duration": 7},
                    {"delay": 3},
                ]}},
                lambda s: s.get("queue", {}).get("total_steps") == 2,
            )
            self.command_status(
                {"queue_control": "stop"},
                lambda s: s.get("queue", {}).get("running") is False,
            )
            self.command_status({"mode": 0, "auto_save": True}, lambda s: s.get("mode") == 0)
            self.command_status({"sensor_sleep": 37}, lambda s: s.get("sensor_sleep_sec") == 37)
            self.command_status({"all": False}, lambda s: s.get("relays") == [False] * RELAY_COUNT)
            before = len(self.mqtt.statuses)
            self.serial.reset()
            self.serial.wait_line(lambda x: "Tasks started." in x, 15)
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                for status in self.mqtt.statuses[before:]:
                    if status.get("uptime_ms", 999999) < 20000:
                        if status.get("mode") != 0 or status.get("sensor_sleep_sec") != 37:
                            raise AssertionError(
                                f"restored mode/sleep mismatch: {status.get('mode')}/"
                                f"{status.get('sensor_sleep_sec')}"
                            )
                        queue_status = self.command_status(
                            {"mode": 2},
                            lambda s: s.get("mode") == 2
                            and s.get("queue", {}).get("total_steps") == 2,
                        )
                        masks = [
                            step.get("relay_mask")
                            for step in queue_status["queue"]["steps"]
                        ]
                        if masks != [8, 0]:
                            raise AssertionError(
                                f"restored queue mismatch: {queue_status['queue']}"
                            )
                        self.command_status(
                            {"mode": 0}, lambda s: s.get("mode") == 0
                        )
                        return (
                            "rebooted; Manual mode, 37 s sleep, and two-step "
                            "queue configuration persisted"
                        )
                time.sleep(0.2)
            raise TimeoutError("post-reboot MQTT status timeout")
        self.case("PERSIST recoverable reboot", run)

    def physical_window(self, seconds: int) -> None:
        self.serial.send("diagnosticApp")
        self.serial.wait_line(lambda line: "[diagnosticApp] ENABLED" in line, 5)
        start_index = len(self.serial.history)
        time.sleep(seconds)
        lines = self.serial.history[start_index:]
        types = sorted({
            match.group(1)
            for line in lines
            for match in [re.search(r"displayRx\s+\| received type=(\S+)", line)]
            if match
            if match.group(1) != "echo"
        })
        failures = [
            line for line in lines
            if "displayRx" in line and ("FAIL" in line or "queue full" in line)
        ]
        started = time.monotonic()
        if failures:
            self.recorder.add(
                "HMI physical controls", "FAIL", f"display RX failures: {failures}",
                started,
            )
        elif types:
            self.recorder.add(
                "HMI physical controls", "PASS",
                f"gateway received HMI command types: {', '.join(types)}", started,
            )
        else:
            self.recorder.add(
                "HMI physical controls", "BLOCKED",
                "no physical touch commands observed during the test window", started,
            )
        self.serial.send("test display")
        echo = self.serial.wait_line(lambda line: line.startswith("DISPLAY: echo="), 5)
        self.recorder.add("HMI post-touch link", "PASS", echo, time.monotonic())


def discover_prefix(serial_link: SerialLink, timeout: float = 18) -> str:
    pattern = re.compile(r"Command topic:\s*([0-9A-Fa-f]+)/command")
    for line in serial_link.history:
        match = pattern.search(line)
        if match:
            return match.group(1)
    line = serial_link.wait_line(lambda text: pattern.search(text) is not None, timeout)
    return pattern.search(line).group(1)  # type: ignore[union-attr]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM14")
    parser.add_argument("--prefix", default="")
    parser.add_argument("--output", type=Path, default=ROOT / "tests" / "results")
    parser.add_argument(
        "--phase",
        choices=(
            "baseline", "automated", "boundaries", "persistence",
            "physical", "cleanup", "all",
        ),
        default="all",
    )
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--physical-seconds", type=int, default=90)
    args = parser.parse_args()

    recorder = Recorder(args.output)
    serial_link: SerialLink | None = None
    mqtt_link: MqttLink | None = None
    report: Path | None = None
    metadata: dict[str, Any] = {"port": args.port, "phase": args.phase}
    exit_code = 1
    try:
        serial_link = SerialLink(args.port, recorder)
        if not args.no_reset:
            serial_link.reset()
        prefix = args.prefix or discover_prefix(serial_link)
        metadata["topic_prefix"] = prefix
        mqtt_link = MqttLink(prefix, recorder)
        mqtt_link.await_fresh_status()
        runner = HilRunner(serial_link, mqtt_link, recorder)
        if args.phase in ("baseline", "all"):
            runner.baseline()
        if args.phase in ("automated", "all"):
            runner.gateway_cases()
            runner.queue_boundaries()
            runner.persistence_reboot()
        elif args.phase == "persistence":
            runner.persistence_reboot()
        elif args.phase == "boundaries":
            runner.queue_boundaries()
        elif args.phase == "physical":
            runner.physical_window(max(10, args.physical_seconds))
        if args.phase in ("cleanup", "all"):
            runner.classify_hub_unavailable()
            runner.case("SAFE final state", runner.final_check)
    except Exception as error:
        recorder.add("HARNESS", "FAIL", str(error), time.monotonic())
    finally:
        if mqtt_link and serial_link:
            try:
                HilRunner(serial_link, mqtt_link, recorder).safe_state()
            except Exception as error:
                recorder.add("EMERGENCY cleanup", "FAIL", str(error), time.monotonic())
        if mqtt_link:
            mqtt_link.close()
        if serial_link:
            serial_link.close()
        exit_code = 0 if not any(r.status == "FAIL" for r in recorder.results) else 1
        report = recorder.close(metadata)
        print(f"Report: {report}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
