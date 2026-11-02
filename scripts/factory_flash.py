#!/usr/bin/env python3
"""Flash the binaries in this folder, one board at a time.

Run it from the factory folder after build_factory.py has filled that folder:

    python flash.py

Name a port for every board and skip the confirmation prompt:

    python flash.py --gateway COM5 --display COM6 --hub COM7 --sensor COM8 --demo COM9 --yes

Type skip instead of a port to leave that board alone. The HMI UART switch
must be toward the PC before the display step, then back toward the gateway.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "manifest.json"


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def espressif_ports() -> list[str]:
    try:
        from serial.tools import list_ports
    except ImportError:
        list_ports = None
    if list_ports is not None:
        found = []
        for port in list_ports.comports():
            if port.vid == 0x303A and port.device:
                found.append(port.device)
        return found
    if os.name != "nt":
        return []
    script = (
        "Get-CimInstance Win32_PnPEntity -Filter \"Name LIKE '%(COM%'\" "
        "| Where-Object { $_.DeviceID -like 'USB\\VID_303A*' } "
        "| ForEach-Object { if ($_.Name -match '\\((COM\\d+)\\)') { $Matches[1] } }"
    )
    try:
        output = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", script],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [line.strip() for line in output.splitlines() if line.strip()]


def find_esptool() -> list[str]:
    names = ("esptool.exe", "esptool") if os.name == "nt" else ("esptool",)
    for name in names:
        found = shutil.which(name)
        if found:
            return [found]
    candidates = [
        Path(r"C:\pio-idf\penv\Scripts\esptool.exe"),
        Path(r"C:\pio-display\penv\Scripts\esptool.exe"),
        Path.home() / ".platformio" / "penv" / "Scripts" / "esptool.exe",
        Path.home() / ".platformio" / "penv" / "bin" / "esptool",
        Path.home() / ".pio-idf" / "penv" / "bin" / "esptool",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return [str(candidate)]
    return [sys.executable, "-m", "esptool"]


def load_manifest() -> list[dict]:
    if not MANIFEST.is_file():
        raise SystemExit(f"Missing {MANIFEST}. Run scripts/build_factory.py first.")
    payload = json.loads(MANIFEST.read_text(encoding="utf-8"))
    devices = payload.get("devices")
    if not isinstance(devices, list) or not devices:
        raise SystemExit(f"{MANIFEST} does not list any devices.")
    return devices


def choose_port(title: str, supplied: str, assume_yes: bool) -> str | None:
    if supplied.lower() == "skip":
        return None
    detected = espressif_ports()
    print(f"Espressif USB ports: {', '.join(detected) if detected else 'none'}")
    if supplied:
        if assume_yes:
            return supplied
        answer = input(
            f"Press Enter to flash {title} on {supplied}, type another port, or type skip: "
        ).strip()
        if answer.lower() == "skip":
            return None
        return answer or supplied
    if assume_yes:
        raise SystemExit(f"{title} needs a port. Pass it on the command line or omit --yes.")
    while True:
        answer = input(f"Port for {title} (example COM5, or skip): ").strip()
        if answer.lower() == "skip":
            return None
        if answer:
            return answer
        print("Type a port, or skip.")


def flash_device(esptool: list[str], device: dict, port: str) -> None:
    command = [
        *esptool,
        "--chip", str(device["chip"]),
        "--port", port,
        "--baud", str(device.get("baud", 460800)),
        "--before", "default-reset",
        "--after", "hard-reset",
        "write-flash",
        "-z",
        "--flash-mode", str(device["flash_mode"]),
        "--flash-freq", str(device["flash_freq"]),
        "--flash-size", str(device.get("flash_size", "detect")),
    ]
    for image in device["images"]:
        path = HERE / str(image["file"])
        if not path.is_file():
            raise SystemExit(f"Missing {path}")
        command.extend([str(image["offset"]), str(path)])
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    print(" ".join(command))
    code = subprocess.call(command, cwd=HERE, env=env)
    if code != 0:
        raise SystemExit(f"{device['title']} flash failed on {port} (exit {code}).")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gateway", default="", help="Gateway USB port, or skip")
    parser.add_argument("--display", default="", help="HMI USB port, or skip")
    parser.add_argument("--hub", default="", help="Zigbee hub USB port, or skip")
    parser.add_argument("--sensor", default="", help="Zigbee sensor USB port, or skip")
    parser.add_argument("--demo", default="", help="H2 demo USB port, or skip")
    parser.add_argument("--yes", action="store_true", help="Do not ask for confirmation when a port is already given")
    args = parser.parse_args(argv)
    supplied = {
        "gateway": args.gateway,
        "display": args.display,
        "hub": args.hub,
        "sensor": args.sensor,
        "demo": args.demo,
    }
    devices = load_manifest()
    missing = []
    for device in devices:
        for image in device["images"]:
            path = HERE / str(image["file"])
            if not path.is_file():
                missing.append(str(path))
    if missing:
        eprint("These factory images are missing:")
        for path in missing:
            eprint(f"  {path}")
        return 1

    esptool = find_esptool()
    print("Flashing one board at a time. Leave the other boards unplugged while a port is written.")
    used: dict[str, str] = {}
    try:
        for index, device in enumerate(devices, start=1):
            title = str(device["title"])
            key = str(device["key"])
            print(f"\n=== Step {index}/{len(devices)}: {title} ===")
            note = str(device.get("note", "")).strip()
            if note:
                print(note)
            port = choose_port(title, supplied.get(key, ""), args.yes)
            if port is None:
                print(f"Skipped {title}.")
                continue
            previous = used.get(port)
            if previous:
                raise SystemExit(
                    f"{port} was already used for {previous}. Unplug that board and pass the port for {title}."
                )
            print(f"Flashing {title} to {port}")
            flash_device(esptool, device, port)
            used[port] = title
            print(f"{title} flashed.")
            if key == "display":
                print("Move the HMI UART switch back toward the gateway before using the screen.")
    except KeyboardInterrupt:
        eprint("\nStopped. The board on the port being written may need this step again.")
        return 1
    print("\nFactory flash finished.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
