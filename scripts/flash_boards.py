#!/usr/bin/env python3
"""Flash the four already-built Shunya images, one microcontroller at a time.

This does not rebuild. Each step waits until that board's USB port is the one
you name, so a shared cable can be moved between boards.

    python scripts/flash_boards.py

Or name every port and skip the prompts:

    python scripts/flash_boards.py --gateway COM5 --display COM12 --hub COM7 --sensor COM8 --yes

Before the HMI step, move the display UART switch to the PC. Move it back
toward the gateway when that step finishes.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shunya_build as sb  # noqa: E402


STEPS = (
    {
        "key": "gateway",
        "title": "Gateway ESP32-S3",
        "env": "gateway_s3",
        "image": sb.REPO_ROOT / ".pio" / "build" / "gateway_s3" / "firmware.bin",
        "core": "gateway",
        "cache": "default",
        "build": sb.REPO_ROOT / ".pio" / "build",
        "note": "Connect the gateway USB port. Leave the hub, sensor, and HMI unplugged.",
    },
    {
        "key": "display",
        "title": "HMI Waveshare ESP32-S3-Touch-LCD-7",
        "env": "display_s3",
        "image": sb.REPO_ROOT / ".pio" / "build-display" / "display_s3" / "firmware.bin",
        "core": "display",
        "cache": "display_cache",
        "build": sb.REPO_ROOT / ".pio" / "build-display",
        "note": (
            "Move the HMI UART switch to the PC side, then connect the HMI USB port. "
            "Move the switch back toward the gateway after this step succeeds."
        ),
    },
    {
        "key": "hub",
        "title": "Zigbee hub ESP32-C6 DevKitC-1",
        "env": "zigbee_hub_c6",
        "image": sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_hub_c6" / "firmware.bin",
        "core": "idf",
        "cache": "default",
        "build": sb.REPO_ROOT / ".pio" / "build-idf",
        "note": "Connect the hub DevKitC-1 USB port. Do not connect the sensor yet.",
    },
    {
        "key": "sensor",
        "title": "Zigbee sensor ESP32-C6-MINI-1",
        "env": "zigbee_sensor_c6",
        "image": sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_sensor_c6" / "firmware.bin",
        "core": "idf",
        "cache": "default",
        "build": sb.REPO_ROOT / ".pio" / "build-idf",
        "note": "Connect the sensor Mini-1 USB port.",
    },
)


def cache_dir(kind: str) -> Path:
    if kind == "default":
        return sb.default_cache()
    return sb.store_root(kind)


def espressif_ports() -> list[str]:
    return sb.list_serial_ports()


def choose_port(title: str, supplied: str, assume_yes: bool) -> str:
    detected = espressif_ports()
    print(f"Espressif USB ports: {', '.join(detected) if detected else 'none'}")
    if supplied:
        if not assume_yes:
            answer = input(f"Press Enter to flash {title} on {supplied}, or type a different port: ").strip()
            return answer or supplied
        return supplied
    if assume_yes:
        raise sb.BuildError(f"{title} needs a port. Pass it on the command line or omit --yes.")
    if len(detected) == 1:
        answer = input(f"Press Enter to flash {title} on {detected[0]}, or type another port: ").strip()
        return answer or detected[0]
    typed = input(f"Type the port for {title} (example COM5): ").strip()
    if not typed:
        raise sb.BuildError(f"No port selected for {title}.")
    return typed


def flash_step(step: dict[str, object], port: str) -> None:
    image = step["image"]
    assert isinstance(image, Path)
    if not image.is_file():
        raise sb.BuildError(f"Missing {image}. Build {step['env']} before flashing.")
    core_kind = str(step["core"])
    code = sb.run_pio(
        ["run", "-e", str(step["env"]), "-t", "nobuild", "-t", "upload", "--upload-port", port],
        sb.store_root(core_kind),
        cache_dir(str(step["cache"])),
        Path(step["build"]),
    )
    if code != 0:
        raise sb.BuildError(f"{step['title']} flash failed on {port} (exit {code}).")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gateway", default="", help="Gateway USB port")
    parser.add_argument("--display", default="", help="HMI USB port")
    parser.add_argument("--hub", default="", help="Zigbee hub USB port")
    parser.add_argument("--sensor", default="", help="Zigbee sensor USB port")
    parser.add_argument("--yes", action="store_true", help="Do not prompt when every port is already given")
    args = parser.parse_args(argv)
    supplied = {
        "gateway": args.gateway,
        "display": args.display,
        "hub": args.hub,
        "sensor": args.sensor,
    }

    missing = [str(step["image"]) for step in STEPS if not Path(step["image"]).is_file()]
    if missing:
        sb.eprint("Build these images before flashing:")
        for path in missing:
            sb.eprint(f"  {path}")
        return 1

    print("Flashing four boards, one at a time. Unplug the previous board before each step.")
    try:
        for index, step in enumerate(STEPS, start=1):
            title = str(step["title"])
            print(f"\n=== Step {index}/{len(STEPS)}: {title} ===")
            print(step["note"])
            port = choose_port(title, supplied[str(step["key"])], args.yes)
            print(f"Flashing {step['env']} to {port}")
            flash_step(step, port)
            print(f"{title} flashed.")
            if step["key"] == "display":
                print("Move the HMI UART switch back toward the gateway before using the screen.")
    except sb.BuildError as error:
        sb.eprint(error)
        return 1
    print("\nAll four images flashed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
