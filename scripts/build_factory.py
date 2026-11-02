#!/usr/bin/env python3
"""Build every Shunya image and save the flash files in factory/.

The folder also receives flash.py. That script asks for each board's USB port
and writes the saved binaries one board at a time.

    python scripts/build_factory.py
    python factory/flash.py

Pass --skip-build to package binaries that are already compiled.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shunya_build as sb  # noqa: E402

FACTORY = sb.REPO_ROOT / "factory"
FLASH_SOURCE = Path(__file__).resolve().parent / "factory_flash.py"


def boot_app0(kind: str) -> Path:
    root = sb.store_root(kind) if kind != "gateway" else sb.HOME / ".platformio"
    return root / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"


def require(path: Path) -> Path:
    if not path.is_file():
        raise sb.BuildError(f"Missing build output: {path}")
    return path


def copy_image(source: Path, name: str) -> dict[str, object]:
    destination = FACTORY / name
    shutil.copy2(require(source), destination)
    return {
        "file": name,
        "bytes": destination.stat().st_size,
        "sha256": sb.sha256_file(destination),
    }


def arduino_device(
    key: str,
    title: str,
    chip: str,
    build_dir: Path,
    boot_app: Path,
    note: str,
) -> dict[str, object]:
    prefix = key
    pieces = [
        ("0x0", build_dir / "bootloader.bin", f"{prefix}-bootloader.bin"),
        ("0x8000", build_dir / "partitions.bin", f"{prefix}-partitions.bin"),
        ("0xe000", boot_app, f"{prefix}-boot_app0.bin"),
        ("0x10000", build_dir / "firmware.bin", f"{prefix}-firmware.bin"),
    ]
    images = []
    for offset, source, name in pieces:
        image = copy_image(source, name)
        image["offset"] = offset
        images.append(image)
    return {
        "key": key,
        "title": title,
        "chip": chip,
        "flash_mode": "dio",
        "flash_freq": "80m",
        "flash_size": "16MB",
        "baud": 460800,
        "note": note,
        "images": images,
    }


def idf_device(key: str, title: str, build_dir: Path, note: str) -> dict[str, object]:
    args_path = require(build_dir / "flasher_args.json")
    args = json.loads(args_path.read_text(encoding="utf-8"))
    settings = args["flash_settings"]
    root_names = {
        "bootloader.bin": "bootloader.bin",
        "partition-table.bin": "partitions.bin",
        "shunya_esp.bin": "firmware.bin",
    }
    images = []
    for offset, relative in args["flash_files"].items():
        source_name = Path(relative).name
        source = build_dir / root_names.get(source_name, source_name)
        if not source.is_file():
            source = build_dir / relative
        image = copy_image(source, f"{key}-{source.name}")
        image["offset"] = offset
        images.append(image)
    images.sort(key=lambda item: int(str(item["offset"]), 16))
    return {
        "key": key,
        "title": title,
        "chip": args["extra_esptool_args"]["chip"],
        "flash_mode": settings["flash_mode"],
        "flash_freq": settings["flash_freq"],
        "flash_size": settings["flash_size"],
        "baud": 460800,
        "note": note,
        "images": images,
    }


def build_all() -> None:
    steps = (
        ("Gateway", lambda: sb.run_pio(
            ["run", "-e", "gateway_s3"],
            sb.store_root("gateway"),
            sb.default_cache(),
            sb.REPO_ROOT / ".pio" / "build",
        )),
        ("HMI", lambda: sb.cmd_display("build", None)),
        ("Zigbee hub", lambda: sb.cmd_idf("hub", "build", None)),
        ("Zigbee sensor", lambda: sb.cmd_idf("sensor", "build", None)),
        ("H2 demo", lambda: sb.cmd_idf("demo", "build", None)),
    )
    for title, action in steps:
        print(f"\n[factory] Building {title}...")
        code = action()
        if code:
            raise sb.BuildError(f"{title} build failed (exit {code}).")


def package() -> None:
    required = [
        sb.REPO_ROOT / ".pio" / "build" / "gateway_s3" / "firmware.bin",
        sb.REPO_ROOT / ".pio" / "build-display" / "display_s3" / "firmware.bin",
        sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_hub_c6" / "flasher_args.json",
        sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_sensor_c6" / "flasher_args.json",
        sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_sensor_h2_demo" / "flasher_args.json",
        boot_app0("gateway"),
        boot_app0("display"),
    ]
    missing = [path for path in required if not path.is_file()]
    if missing:
        raise sb.BuildError("Missing build output:\n" + "\n".join(f"  {path}" for path in missing))
    if FACTORY.exists():
        for child in FACTORY.iterdir():
            if child.is_file():
                child.unlink()
    FACTORY.mkdir(parents=True, exist_ok=True)
    devices = [
        arduino_device(
            "gateway",
            "Gateway ESP32-S3",
            "esp32s3",
            sb.REPO_ROOT / ".pio" / "build" / "gateway_s3",
            boot_app0("gateway"),
            "Connect the gateway USB port.",
        ),
        arduino_device(
            "display",
            "HMI Waveshare ESP32-S3-Touch-LCD-7",
            "esp32s3",
            sb.REPO_ROOT / ".pio" / "build-display" / "display_s3",
            boot_app0("display"),
            "Move the HMI UART switch to the PC side, then connect the HMI USB port.",
        ),
        idf_device(
            "hub",
            "Zigbee hub ESP32-C6 DevKitC-1",
            sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_hub_c6",
            "Connect the hub DevKitC-1 USB port.",
        ),
        idf_device(
            "sensor",
            "Zigbee sensor ESP32-C6-MINI-1",
            sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_sensor_c6",
            "Connect the sensor Mini-1 USB port.",
        ),
        idf_device(
            "demo",
            "H2 sensor demo ESP32-H2 DevKitM-1",
            sb.REPO_ROOT / ".pio" / "build-idf" / "zigbee_sensor_h2_demo",
            "Connect the H2 demo USB port. Type skip if this board is not being programmed.",
        ),
    ]
    manifest = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "gateway_version": sb.version_from(sb.REPO_ROOT / "src" / "gateway" / "config.h", "CURRENT_VERSION"),
        "hmi_version": sb.version_from(sb.REPO_ROOT / "src" / "display" / "display_config.h", "HMI_FIRMWARE_VERSION"),
        "devices": devices,
    }
    (FACTORY / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(FLASH_SOURCE, FACTORY / "flash.py")
    print(f"\n[factory] Saved binaries and flash.py in {FACTORY}")
    for item in sorted(FACTORY.iterdir()):
        print(f"  {item.name:32} {item.stat().st_size}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skip-build", action="store_true", help="Package the binaries already in .pio")
    args = parser.parse_args(argv)
    try:
        if not args.skip_build:
            build_all()
        package()
    except sb.BuildError as error:
        sb.eprint(error)
        return 1
    print("\nFlash the boards with: python factory/flash.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
