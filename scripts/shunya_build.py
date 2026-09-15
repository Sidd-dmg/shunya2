#!/usr/bin/env python3
"""OS-independent wrappers for isolated PlatformIO stores and the HMI simulator.

Windows, macOS, and Linux share this entry point. Thin .ps1/.sh files only
forward arguments.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
IS_WINDOWS = os.name == "nt"


class BuildError(Exception):
    pass


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def which(name: str) -> str | None:
    return shutil.which(name)


def find_pio() -> Path:
    names = ("pio.exe", "pio") if IS_WINDOWS else ("pio",)
    candidates: list[Path] = []
    for name in names:
        candidates.append(HOME / ".platformio" / "penv" / ("Scripts" if IS_WINDOWS else "bin") / name)
    path_pio = which("pio")
    if path_pio:
        candidates.append(Path(path_pio))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise BuildError(
        "PlatformIO Core not found. Install the PlatformIO IDE extension or "
        "`pip install platformio`, then ensure ~/.platformio/penv exists. "
        "Do not mix the system Core with the isolated IDF/HMI stores."
    )


def find_python() -> str:
    if IS_WINDOWS:
        for name in ("py", "python", "python3"):
            if which(name):
                return name
    else:
        for name in ("python3", "python"):
            if which(name):
                return name
    return sys.executable


def store_root(kind: str) -> Path:
    """Isolated PlatformIO package stores. Short Windows roots keep MAX_PATH."""
    env_override = os.environ.get(f"SHUNYA_{kind.upper()}_STORE")
    if env_override:
        return Path(env_override)
    if IS_WINDOWS:
        mapping = {
            "idf": Path(r"C:\pio-idf"),
            "display": Path(r"C:\pio-display"),
            "display_cache": Path(r"C:\pio-display-cache"),
            "gateway": HOME / ".platformio",
        }
    else:
        mapping = {
            "idf": HOME / ".pio-idf",
            "display": HOME / ".pio-display",
            "display_cache": HOME / ".pio-display-cache",
            "gateway": HOME / ".platformio",
        }
    return mapping[kind]


def default_cache() -> Path:
    return HOME / ".platformio" / ".cache"


def apply_env(core: Path, cache: Path, build: Path) -> dict[str, str | None]:
    previous = {
        "PLATFORMIO_CORE_DIR": os.environ.get("PLATFORMIO_CORE_DIR"),
        "PLATFORMIO_CACHE_DIR": os.environ.get("PLATFORMIO_CACHE_DIR"),
        "PLATFORMIO_BUILD_DIR": os.environ.get("PLATFORMIO_BUILD_DIR"),
        "UV_SYSTEM_CERTS": os.environ.get("UV_SYSTEM_CERTS"),
    }
    os.environ["PLATFORMIO_CORE_DIR"] = str(core)
    os.environ["PLATFORMIO_CACHE_DIR"] = str(cache)
    os.environ["PLATFORMIO_BUILD_DIR"] = str(build)
    if IS_WINDOWS:
        os.environ["UV_SYSTEM_CERTS"] = "true"
    return previous


def restore_env(previous: dict[str, str | None]) -> None:
    for key, value in previous.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value


def run_pio(args: list[str], core: Path, cache: Path, build: Path) -> int:
    previous = apply_env(core, cache, build)
    try:
        print(f"[shunya-build] {' '.join(args)} (core: {core})")
        return subprocess.call([str(find_pio()), *args], cwd=REPO_ROOT)
    finally:
        restore_env(previous)


def list_serial_ports() -> list[str]:
    ports: list[str] = []
    if IS_WINDOWS:
        cmd = [
            "powershell",
            "-NoProfile",
            "-Command",
            (
                "Get-CimInstance Win32_PnPEntity -Filter \"Name LIKE '%(COM%'\" "
                "| Where-Object { $_.DeviceID -like 'USB\\VID_303A*' } "
                "| ForEach-Object { if ($_.Name -match '\\((COM\\d+)\\)') { $Matches[1] } }"
            ),
        ]
        try:
            output = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
            ports = [line.strip() for line in output.splitlines() if line.strip()]
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
        return ports

    patterns = ["ttyACM*", "ttyUSB*"] if sys.platform.startswith("linux") else [
        "cu.usbmodem*",
        "cu.usbserial*",
        "cu.wchusbserial*",
        "cu.SLAB_USBtoUART*",
    ]
    seen: set[str] = set()
    for pattern in patterns:
        for path in Path("/dev").glob(pattern):
            name = str(path)
            if name not in seen:
                seen.add(name)
                ports.append(name)
    return ports


def resolve_port(requested: str | None, needed: bool) -> str | None:
    if requested:
        return requested
    if not needed:
        return None
    ports = list_serial_ports()
    if len(ports) == 1:
        print(f"[shunya-build] using {ports[0]}")
        return ports[0]
    if not ports:
        raise BuildError(
            "No serial device found. Pass --port (COMx on Windows, "
            "/dev/ttyACM0 on Linux, /dev/cu.usbmodem* on macOS)."
        )
    raise BuildError(
        "Several serial devices are connected ("
        + ", ".join(ports)
        + "). Re-run with --port to pick one."
    )


def bootlog(port: str) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        raise BuildError(
            "pyserial is required for bootlog. Install it with "
            f"`{find_python()} -m pip install pyserial`."
        ) from None

    print(f"[shunya-build] resetting {port} and capturing boot output...")
    ser = serial.Serial(port, 115200, timeout=1)
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.25)
        ser.rts = False
        time.sleep(6)
        data = ser.read(ser.in_waiting or 1)
        extra = ser.read(4096)
        text = (data + extra).decode("utf-8", errors="replace")
    finally:
        ser.close()
    if text.strip():
        print(text, end="" if text.endswith("\n") else "\n")
        return 0
    raise BuildError(
        "No output after reset. The board is not running, or the console is not on this port."
    )


IDF_ENVS = {
    "hub": "zigbee_hub_c6",
    "sensor": "zigbee_sensor_c6",
    "demo": "zigbee_sensor_h2_demo",
}


def cmd_idf(target: str, action: str, port: str | None) -> int:
    env_name = IDF_ENVS[target]
    port = resolve_port(port, action in {"upload", "monitor", "bootlog"})
    if action == "bootlog":
        if not port:
            raise BuildError("bootlog requires --port")
        return bootlog(port)

    pio_args = {
        "build": ["run", "-e", env_name],
        "upload": ["run", "-e", env_name, "-t", "upload"],
        "clean": ["run", "-e", env_name, "-t", "fullclean"],
        "monitor": ["device", "monitor", "-e", env_name],
    }[action]
    if port:
        pio_args += ["--port" if action == "monitor" else "--upload-port", port]
    return run_pio(
        pio_args,
        store_root("idf"),
        default_cache(),
        REPO_ROOT / ".pio" / "build-idf",
    )


def ensure_display_framework(core: Path) -> None:
    framework = core / "packages" / "framework-arduinoespressif32"
    marker = framework / "tools" / "pioarduino-build.py"
    if framework.exists() and not marker.exists():
        print(
            f"[shunya-build] Arduino 2.x framework found in {core}; "
            "removing it so Arduino 3.x is reinstalled."
        )
        shutil.rmtree(framework, ignore_errors=True)


def cmd_display(action: str, port: str | None) -> int:
    core = store_root("display")
    ensure_display_framework(core)
    port = resolve_port(port, action in {"upload", "monitor"})
    pio_args = {
        "build": ["run", "-e", "display_s3"],
        "upload": ["run", "-e", "display_s3", "-t", "upload"],
        "monitor": ["device", "monitor", "-e", "display_s3"],
        "clean": ["run", "-e", "display_s3", "-t", "fullclean"],
    }[action]
    if port:
        pio_args += ["--port" if action == "monitor" else "--upload-port", port]
    return run_pio(
        pio_args,
        core,
        store_root("display_cache"),
        REPO_ROOT / ".pio" / "build-display",
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_artifact(source: Path, dest: Path, target: str, artifacts: list[dict]) -> None:
    if not source.is_file():
        print(f"[shunya-build] skipping {dest.name}: {source} is missing")
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
    artifacts.append(
        {
            "file": dest.name,
            "target": target,
            "bytes": dest.stat().st_size,
            "sha256": sha256_file(dest),
        }
    )


def version_from(path: Path, macro: str) -> str:
    match = re.search(rf'{macro}\s+"([^"]+)"', path.read_text(encoding="utf-8"))
    return match.group(1) if match else ""


def cmd_release(skip_build: bool, output_root: str | None) -> int:
    if not skip_build:
        previous = apply_env(
            store_root("gateway"),
            default_cache(),
            REPO_ROOT / ".pio" / "build",
        )
        try:
            print("[shunya-build] Building gateway_s3...")
            code = subprocess.call([str(find_pio()), "run", "-e", "gateway_s3"], cwd=REPO_ROOT)
            if code:
                return code
        finally:
            restore_env(previous)
        code = cmd_display("build", None)
        if code:
            return code

    gateway_build = REPO_ROOT / ".pio" / "build" / "gateway_s3"
    display_build = REPO_ROOT / ".pio" / "build-display" / "display_s3"
    for required in (gateway_build / "firmware.bin", display_build / "firmware.bin"):
        if not required.is_file():
            raise BuildError(f"Missing build output: {required}")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    release_dir = Path(output_root) if output_root else REPO_ROOT / "dist" / "firmware" / stamp
    if output_root:
        release_dir = Path(output_root) / stamp
    release_dir.mkdir(parents=True, exist_ok=True)

    artifacts: list[dict] = []
    gateway_boot_app0 = (
        HOME / ".platformio" / "packages" / "framework-arduinoespressif32"
        / "tools" / "partitions" / "boot_app0.bin"
    )
    hmi_boot_app0 = (
        store_root("display") / "packages" / "framework-arduinoespressif32"
        / "tools" / "partitions" / "boot_app0.bin"
    )
    copies = [
        (gateway_build / "firmware.bin", "gateway-firmware.bin", "gateway_s3 OTA/application"),
        (gateway_build / "bootloader.bin", "gateway-bootloader.bin", "gateway_s3 offset 0x0000"),
        (gateway_build / "partitions.bin", "gateway-partitions.bin", "gateway_s3 offset 0x8000"),
        (gateway_boot_app0, "gateway-boot_app0.bin", "gateway_s3 offset 0xE000"),
        (display_build / "firmware.bin", "hmi-firmware.bin", "display_s3 OTA/application"),
        (display_build / "firmware.factory.bin", "hmi-factory.bin", "display_s3 complete initial-flash image"),
        (display_build / "bootloader.bin", "hmi-bootloader.bin", "display_s3 offset 0x0000"),
        (display_build / "partitions.bin", "hmi-partitions.bin", "display_s3 offset 0x8000"),
        (hmi_boot_app0, "hmi-boot_app0.bin", "display_s3 offset 0xE000"),
    ]
    for source, name, target in copies:
        copy_artifact(source, release_dir / name, target, artifacts)

    manifest = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "gateway_version": version_from(REPO_ROOT / "src" / "gateway" / "config.h", "CURRENT_VERSION"),
        "hmi_version": version_from(REPO_ROOT / "src" / "display" / "display_config.h", "HMI_FIRMWARE_VERSION"),
        "artifacts": artifacts,
    }
    (release_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[shunya-build] Binaries and manifest: {release_dir}")
    for item in sorted(release_dir.iterdir()):
        print(f"  {item.name:28} {item.stat().st_size}")
    return 0


def simulator_setup() -> int:
    if IS_WINDOWS:
        ps1 = REPO_ROOT / "scripts" / "hmi-simulator.ps1"
        return subprocess.call(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1), "setup"]
        )
    if sys.platform == "darwin":
        print("Install simulator deps with Homebrew:")
        print("  brew install cmake ninja sdl2 pkg-config")
    else:
        print("Install simulator deps with apt (Debian/Ubuntu):")
        print("  sudo apt-get update && sudo apt-get install -y cmake ninja-build g++ pkg-config libsdl2-dev git")
    print("Then run: python3 scripts/shunya_build.py simulator build")
    return 0


def cmd_simulator(action: str) -> int:
    sim = REPO_ROOT / "simulator"
    build = sim / "build"
    if action == "setup":
        return simulator_setup()
    if action == "clean":
        shutil.rmtree(build, ignore_errors=True)
        print("[shunya-build] Native simulator build directory removed.")
        return 0

    if IS_WINDOWS and not which("cmake"):
        ps1 = REPO_ROOT / "scripts" / "hmi-simulator.ps1"
        return subprocess.call(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1), action]
        )

    for tool in ("cmake",):
        if not which(tool):
            raise BuildError(f"{tool} is not on PATH. Run: python3 scripts/shunya_build.py simulator setup")

    cmake_cmd = ["cmake", "-S", str(sim), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug"]
    if which("ninja"):
        cmake_cmd = [
            "cmake", "-S", str(sim), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
        ]
    print("[shunya-build] Configuring LVGL 8.4 native simulator...")
    code = subprocess.call(cmake_cmd)
    if code:
        return code
    code = subprocess.call(["cmake", "--build", str(build), "--parallel"])
    if code or action == "build":
        return code

    exe = build / ("shunya-hmi-simulator.exe" if IS_WINDOWS else "shunya-hmi-simulator")
    if action == "run":
        return subprocess.call([str(exe)])
    if action == "test":
        return subprocess.call([str(exe), "--self-test"])
    if action == "capture":
        dashboard = REPO_ROOT / "hmi" / "simulator-dashboard.bmp"
        programs = REPO_ROOT / "hmi" / "simulator-programs.bmp"
        return subprocess.call([str(exe), "--capture", str(dashboard), str(programs)])
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Cross-platform Shunya firmware helpers")
    sub = parser.add_subparsers(dest="cmd", required=True)

    idf = sub.add_parser("idf", help="Zigbee hub / sensor / H2 demo")
    idf.add_argument("target", choices=sorted(IDF_ENVS))
    idf.add_argument("action", choices=["build", "upload", "monitor", "clean", "bootlog"])
    idf.add_argument("--port", "-p", default="")

    display = sub.add_parser("display", help="Waveshare HMI")
    display.add_argument("action", choices=["build", "upload", "monitor", "clean"])
    display.add_argument("--port", "-p", default="")

    release = sub.add_parser("release", help="Package gateway + HMI binaries")
    release.add_argument("--skip-build", action="store_true")
    release.add_argument("--output", default="")

    sim = sub.add_parser("simulator", help="Native LVGL HMI simulator")
    sim.add_argument("action", choices=["setup", "build", "run", "test", "capture", "clean"])
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "idf":
            return cmd_idf(args.target, args.action, args.port or None)
        if args.cmd == "display":
            return cmd_display(args.action, args.port or None)
        if args.cmd == "release":
            return cmd_release(args.skip_build, args.output or None)
        if args.cmd == "simulator":
            return cmd_simulator(args.action)
    except BuildError as error:
        eprint(error)
        return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
