# Shunya user manual

Operator guide for firmware **4.0.1** (gateway) and HMI **1.2.0**. This is the
step-by-step manual for assembling, flashing, pairing, and running a Shunya
stack. Developer internals, OTA publishing, and the full test matrix live in
[`README.md`](README.md) and [`tests/system_test_cases.csv`](tests/system_test_cases.csv).

---

## 1. What you are building

Four pieces of hardware work together:

| Piece | Board | Firmware env | Job |
|---|---|---|---|
| Gateway | 4D Systems ESP32-S3 (16 MB) | `gateway_s3` | Relays, Wi-Fi, MQTT, modes, HMI/hub UART |
| Zigbee hub | ESP32-C6 DevKitC-1 | `zigbee_hub_c6` | Zigbee coordinator + UART to gateway |
| Production sensor | ESP32-C6 + DHT22 | `zigbee_sensor_c6` | Sleepy Zigbee end device + temperature/humidity/battery |
| Local display | Waveshare ESP32-S3-Touch-LCD-7 | `display_s3` | 800×480 touch HMI |

Optional lab sensor: **ESP32-H2-DevKitM-1 (4 MB)** with `zigbee_sensor_h2_demo`.
It has no DHT. You type temperature, humidity, and optional battery % in the serial monitor.

Up to **16** sensors. Logical IDs are always `1–16` on MQTT, automation, EEPROM,
and the HMI.

```
[DHT C6 / H2 demo]  --Zigbee sleepy ZED-->  [C6 coordinator]
                                                 |
                                          UART1 115200
                                                 v
[Waveshare 7"]  <--UART2 CRC/ACK-->  [S3 gateway] --GPIO--> Relays 1–4
                                         |
                                       MQTT
```

---

## 2. Safety and common rules

- Power both ends from their own supplies. Join **GND only** plus the two 3.3 V
  UART wires. Never cross 5 V outputs.
- **One USB cable at a time** unless you pass an explicit `--upload-port` /
  `-Port`. PlatformIO will otherwise pick Bluetooth or Intel AMT ports.
- **Never run two firmware builds at once.** Gateway, Zigbee, and HMI use
  incompatible toolchains. Concurrent installs corrupt packages.
- Always use the wrappers for Zigbee and HMI. Do not call system `pio` for
  those targets.
- Zigbee nodes log at boot and on events only. A silent monitor is usually an
  idle board, not a dead one. Press reset, or use `-Action bootlog`.
- Pairing is **Zigbee permit-join**. Sensors only join while the 120 s window
  is open. Battery percentage and report interval (sleep) are shown on MQTT/HMI.

---

## 3. Hardware wiring

### Gateway (S3)

- Relays: GPIO **8, 18, 16, 15** (HIGH = on)
- Boot / hold actions: GPIO **0**
- USB console: 115200
- HMI UART2: **RX 44 / TX 43**
- Hub UART1: **RX 4 / TX 5**

### Gateway ↔ Zigbee hub

Cross TX/RX. Common GND.

```
ESP32-S3 gateway                 ESP32-C6 hub
GPIO 5 TX   ------------------>  GPIO 4 RX
GPIO 4 RX   <------------------  GPIO 5 TX
GND           ------------------  GND
```

Hub debug console is UART0 / USB-Serial-JTAG.

### Gateway ↔ Waveshare HMI

Set the Waveshare UART selector so GPIO 43/44 go to the external header.

```
Waveshare HMI                    ESP32-S3 gateway
GPIO 43 TX  ------------------>  GPIO 44 RX
GPIO 44 RX  <------------------  GPIO 43 TX
GND         -------------------  GND
```

### DHT sensor (ESP32-C6)

Default in `src/sensor/sensor_config.h`: DHT22 on GPIO 6.

```
DHT22 (or DHT11)
  VCC  →  3.3 V
  GND  →  GND
  DATA →  GPIO 6   (add 4.7 kΩ to 3.3 V if checksums fail)
BOOT   →  GPIO 9   hold 3 s = factory-reset Zigbee state
BAT    →  GPIO 2   optional ADC divider for battery % (2× scale, 3.0–4.2 V)
```

For DHT11, set `#define DHT_MODEL 11` and rebuild `zigbee_sensor_c6`.

---

## 4. Tools you need

1. [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html)
   or the PlatformIO IDE extension (this creates `penv`).
2. USB drivers for the 4D / S3 programmer and ESP32-C6 / H2 boards.
3. Python 3 (comes with PlatformIO).
4. Espressif **ESP BLE Provisioning** on a phone (first Wi-Fi only).
5. Any MQTT 3.1.1 client (MQTT Explorer, mosquitto, Home Assistant).

Confirm PlatformIO:

```bash
pio --version
python3 --version
```

On Linux, add your user to `dialout` so serial ports work without root, then
log out and back in. First Zigbee build creates `C:\pio-idf` (Windows) or
`~/.pio-idf` (macOS/Linux) and can take several minutes. The first HMI build
downloads Arduino 3.x into `C:\pio-display` or `~/.pio-display`.

---

## 5. Find the serial port

```bash
pio device list
```

Use Espressif USB devices (VID `303A`). Pass `--port` on every upload when more
than one board is connected.

Examples: Windows `COM5` / `COM7`, Linux `/dev/ttyACM0`, macOS `/dev/cu.usbmodem1101`.

---

## 6. Build and flash from source

Work from the repository root. Flash **one board**, unplug or close its
monitor, then the next. The same commands work on Windows, macOS, and Linux
(`python` instead of `python3` is fine on Windows).

Full production set: gateway + HMI + C6 hub + C6 DHT. For a bench demo,
replace the DHT with the H2 serial sensor.

### 6.1 Gateway (ESP32-S3)

```powershell
pio run -e gateway_s3
pio run -e gateway_s3 -t upload --upload-port COM5
pio device monitor -e gateway_s3 --port COM5
```

First flash of this image also writes the 16 MB dual-OTA + SPIFFS table.
Without that table, HTTP OTA later reports `No OTA partition found`.

Healthy boot fragment:

```text
=== ESP-B Gateway (Zigbee hub + FreeRTOS) ===
Firmware Version: 4.0.1
Connecting to saved WiFi...
WiFi connected
IP address: 10.72.151.139
Tasks started. Command topic: 1020BA02BBE4/command
```

### 6.2 Zigbee hub (ESP32-C6)

```bash
python3 scripts/shunya_build.py idf hub build
python3 scripts/shunya_build.py idf hub upload --port COM7
python3 scripts/shunya_build.py idf hub monitor --port COM7
```

If the monitor is silent, the board is probably already running:

```bash
python3 scripts/shunya_build.py idf hub bootlog --port COM7
```

Healthy hub:

```text
I (...) hub_main: ESP32-C6 Zigbee hub started, UART GPIO RX4 TX5
I (...) zb_coord: Formed network PAN 0x.... ch ...
```

The coordinator is up immediately. Permit-join stays **closed** until pairing.

### 6.3 DHT sensor (ESP32-C6)

```bash
python3 scripts/shunya_build.py idf sensor build
python3 scripts/shunya_build.py idf sensor upload --port COM9
python3 scripts/shunya_build.py idf sensor monitor --port COM9
```

Expect `DHT22 Zigbee sensor on GPIO 6`. Open hub pairing so it can join as a
sleepy end device. Default report interval is **20 s** (configurable).

### 6.4 H2 serial demo (optional)

**Only** an ESP32-H2-DevKitM-1 with 4 MB flash. Do not put this image on the
C6 hub or C6 DHT.

```bash
python3 scripts/shunya_build.py idf demo build
python3 scripts/shunya_build.py idf demo upload --port COM15
python3 scripts/shunya_build.py idf demo monitor --port COM15
```

Then type one of:

```text
set 24.50 55.25 87
24.50 55.25
```

Temperature `-40..80` °C, humidity `0..100` %, optional battery `0..100` %.
No units, commas, or JSON. Values are held until join, then sent immediately
and every report interval.

### 6.5 Waveshare HMI

```bash
python3 scripts/shunya_build.py display build
python3 scripts/shunya_build.py display upload --port COM12
python3 scripts/shunya_build.py display monitor --port COM12
```

Expect `=== Shunya Waveshare 7-inch HMI ===` plus `Firmware:` and PSRAM size.

### 6.6 Clean rebuilds

```bash
python3 scripts/shunya_build.py idf hub clean
python3 scripts/shunya_build.py idf sensor clean
python3 scripts/shunya_build.py idf demo clean
python3 scripts/shunya_build.py display clean
```

A Zigbee partition-table change needs a **full** upload, not app-only.

### 6.7 Package gateway + HMI binaries

```powershell
.\scripts\build-release-binaries.ps1
```

Outputs land in `dist\firmware\<timestamp>\` with `manifest.json` (versions,
sizes, SHA-256). `-SkipBuild` packages already-built images.

Raw build paths:

| Target | File |
|---|---|
| Gateway app | `.pio\build\gateway_s3\firmware.bin` |
| HMI app | `.pio\build-display\display_s3\firmware.bin` |
| HMI full image | `.pio\build-display\display_s3\firmware.factory.bin` |
| Zigbee hub | `.pio\build-idf\zigbee_hub_c6\firmware.bin` |
| C6 DHT | `.pio\build-idf\zigbee_sensor_c6\firmware.bin` |
| H2 demo | `.pio\build-idf\zigbee_sensor_h2_demo\firmware.bin` |

---

## 7. Flash packaged binaries without compiling

Use a `dist\firmware\<timestamp>\` folder plus `esptool`. ESP32-S3 bootloader
offset is `0x0000`, not `0x1000`.

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool4 = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$esptool5 = "C:\pio-display\packages\tool-esptoolpy\esptool.py"
cd dist\firmware\<timestamp>
```

Gateway store uses esptool **v4** (`write_flash`). HMI store uses **v5**
(`write-flash`).

### Complete gateway

```powershell
& $py $esptool4 --chip esp32s3 --port COM5 --baud 921600 `
    --before default_reset --after hard_reset `
    write_flash -z --flash_mode keep --flash_freq keep --flash_size keep `
    0x0000 gateway-bootloader.bin `
    0x8000 gateway-partitions.bin `
    0xE000 gateway-boot_app0.bin `
    0x10000 gateway-firmware.bin
```

### Complete HMI (merged factory image)

```powershell
& $py $esptool5 --chip esp32s3 --port COM7 --baud 921600 `
    --before default-reset --after hard-reset `
    write-flash -z --flash-mode keep --flash-freq keep --flash-size keep `
    0x0000 hmi-factory.bin
```

Never flash `hmi-factory.bin` at `0x10000` and never use it as an OTA payload.
OTA payloads are only `gateway-firmware.bin` and `hmi-firmware.bin`.

Always write `boot_app0.bin` together with an app at `0x10000`. After a
previous OTA the chip may still boot `app1`; `boot_app0.bin` forces `app0`.

If 921600 fails on a long cable, drop baud to 460800. Compare SHA-256 in
`manifest.json` before flashing a copied release.

Zigbee boards: prefer `python3 scripts/shunya_build.py idf … upload`
so `partitions_zigbee.csv` is written. An app-only write cannot replace an
old Thread partition table.

---

## 8. First boot

1. Power the S3 gateway and C6 collector together.
2. Open gateway USB at **115200**. Note `Firmware Version: 4.0.1` and the
   command topic `{MAC}/command` (MAC **without** colons).
3. If no Wi-Fi is stored, BLE provisioning starts.
   - App: Espressif **ESP BLE Provisioning**
   - Name: `Shunya ` + MAC **with** colons
   - Proof of possession: `abcd1234`
4. After DHCP, MQTT connects to the compiled broker (plaintext **1883**).
5. Hub UART should pass `test hub`. The HMI should leave “link lost”.
6. NTP uses `pool.ntp.org` with IST offset **+19800**. Time stays `N/A`
   until sync.

A gateway with no hub still runs Wi-Fi, MQTT, and relays. Pairing and live
sensor data will not appear until UART to the collector works.

---

## 9. Prove the UARTs before pairing

On the gateway USB console:

```text
help
test hub
test display
stats
```

| Command | Pass means |
|---|---|
| `test hub` | Ping ACK, unique echo, PAN/channel/node count in 500–800 ms |
| `test display` | Versioned echo round-trip within 1 s |
| `test display-tx` | Snapshot reaches the panel |
| `test display-loop` | HMI unplugged, jumper gateway **GPIO43 → GPIO44**, echo returns |
| `test all` | Hub pass + snapshot queued |
| `stats` | TX/RX, CRC errors, online flag, PAN, channel |

Hub console should print `ping` and `echo N bytes` at the same time.

Live tracing:

```text
diagnosticApp
```

`diagnosticApp off` stops it. `diagnosticApp status` reports the flag.

A healthy HMI relay click looks like:

```text
displayRx        | received type=RELAY seq=... len=2
displayRx        | queued type=RELAY ...
relay/request    | queued relay=... state=...
mode/display     | processed type=RELAY ... latency=...ms
relay/apply      | relay=... state=ON source=0
```

If the first line never appears, the touch event never reached the gateway.

---

## 10. Pairing (Zigbee permit-join)

### What pairing actually does

The C6 hub is a Zigbee **coordinator**. Sensors are sleepy **end devices**.
They do **not** join until you open a **120-second permit-join window**.

1. Unknown sensor starts network steering while permit-join is open.
2. If a slot is free, the hub assigns ID **1–16**, stores IEEE in NVS, and
   tells the gateway.
3. Gateway saves IEEE + short address in EEPROM and shows the node on MQTT/HMI.
4. Sensor reports temperature, humidity, and battery. Default interval is
   **20 s**, clamped **5–3600 s**. The hub writes the interval to each node
   after join and after each report (while the ZED is still awake).
5. Reports from unknown IEEE addresses are ignored.

Maximum **16** nodes. A 17th join is rejected.

### Start pairing (any one method)

**A. Gateway BOOT button (GPIO 0)** — hold ≥ 1 s, release **before** 5 s.

**B. HMI** — Sensors tab → **OPEN**. Countdown appears. **CLOSE** ends it.

**C. MQTT** on `{MAC}/command`:

```json
{"pair": true}
```

Close early:

```json
{"pair": false}
```

Gateway log:

```text
Pairing mode enabled (Zigbee permit-join requested)
```

Hub log:

```text
pair open request, 120 s
permit join 120 s
```

Sensor log after success:

```text
Joined network PAN 0x.... ch ...
```

Then MQTT `{MAC}/status` lists the node, `{MAC}/node/1/data` starts updating
(with `battery` when known), and the HMI Sensors tab shows temperature,
humidity, battery, link quality, and age.

### Report interval (sleep)

Global for all sensors. Clamped to 5–3600 seconds.

**HMI** — Sensors tab slider (5–600 s on the panel). Release the knob to send.

**MQTT**:

```json
{"sensor_sleep": 60}
```

Status includes `sensor_sleep_sec`. Battery is omitted when unknown (`0xFF`).

### Worked example: first DHT sensor

1. `test hub` passes.
2. Power the DHT C6. Open pairing for 120 s.
3. Within a few seconds the hub assigns slot 1.
4. Wait up to one sleep interval for the first DHT sample. Empty T/H before
   that is normal.

### Worked example: H2 demo as node 2

1. Keep pairing open or open it again.
2. On the H2 monitor: `set 24.5 55 87` then Enter.
3. After join, MQTT/HMI show 24.50 °C / 55.00 % and 87 % battery.

### Unpair

| Action | Result |
|---|---|
| HMI Sensors → forget one row | Removes that logical ID (the displayed node, not “row 1”) |
| HMI **FORGET ALL** | Clears every paired node |
| BOOT hold **10–20 s** | Forget all, reboot |
| BOOT hold **≥ 20 s** | Full factory reset (relays off, EEPROM wiped, Wi-Fi erased) |
| Sensor BOOT (GPIO 9) **3 s** | Erases that sensor’s Zigbee NVS; permit-join required again |

Forget is gateway-authoritative even if the hub is unplugged. When the hub
UART returns, stale hub slots receive deferred `FORGET_NODE`.

On the Sensors tab, Previous/Next page four nodes at a time (up to four
pages). Forget always uses the **node ID on that row**.

---

## 11. Using the HMI

Five tabs. The gateway is authoritative; the panel never writes EEPROM or GPIO
directly.

| Tab | What you do |
|---|---|
| **Dashboard** | Link / Wi-Fi / MQTT / Zigbee-hub chips. First two active sensors as gauges, with battery. |
| **Relays** | Four 120 px toggles + all-off. Accepted only in **Manual**. |
| **Sensors** | Four rows per page, pairing countdown, sleep slider, forget one / forget all. |
| **Programs** | Choose Manual / Automation / Queue / Hybrid and Apply. Tables are read-only. |
| **System** | Versions, uptime, RSSI, PAN/channel, OTA, logs, link tests, Wi-Fi reset, brightness. |

Relay taps outside Manual show a toast and are NACKed. Destructive actions
ask for confirmation.

USB `[HMI PERF]` every five seconds reports FPS, dirty pixels, frame time,
flush time, idle, heap, and `internal=` SRAM. Keep internal RAM above about
**45 KB** or HMI OTA will refuse to start.

---

## 12. Operating modes (with examples)

Replace `{MAC}` with the gateway MAC printed at boot, no colons. Example
topic: `1020BA02BBE4/command`.

| Mode | Number | Who drives relays |
|---|---|---|
| Manual | 0 | You (HMI or MQTT) |
| Automation | 1 | Sensor rules |
| Queue | 2 | Timed steps |
| Hybrid | 3 | Automation lock + queue for the rest |

Subscribe to `{MAC}/status` (retained, every 10 s) and `{MAC}/node/{id}/data`.

### Switch mode

```json
{"mode": 0}
{"mode": 1, "auto_save": true}
{"mode": 2}
{"mode": 3}
```

`auto_save` writes the mode to EEPROM. Leaving a looping queue with
`"transition": "graceful"` disables loop and waits; other switches are
immediate and turn relays off unless entering Manual.

### Manual — turn pump 1 on, pump 2 off

HMI: Programs → Manual → Apply, then Relays.

MQTT:

```json
{"mode": 0}
{"relay1": 1, "relay2": 0}
{"all": 1}
```

States are saved. Individual HMI buttons are rejected in other modes.

### Automation — fan on node 1 above 28 °C

```json
{"mode": 1, "auto_save": true}
{
  "automation": {
    "rules": [
      {
        "node": 1,
        "relay": 1,
        "logic": "OR",
        "temp": {"min": 20, "max": 28},
        "humidity": {"min": 40, "max": 70}
      }
    ]
  }
}
```

Hysteresis: above `max` → ON, below `min` → OFF, in-band keeps last state.
3 s debounce. A node silent for **5 minutes** is ignored. Clear with
`{"automation": {"clear": true}}`. Up to 16 rules. `node` and `relay` are
1-based.

Drive this from the H2 demo: `set 29.0 50` then `set 19.0 50`.

### Queue — 30 s valves 1+2, 10 s pause, 15 s valve 3

```json
{"mode": 2}
{
  "queue": {
    "loop": true,
    "auto_start": true,
    "steps": [
      {"relays": [1, 2], "duration": 30},
      {"delay": 10},
      {"relay": 3, "duration": 15}
    ]
  }
}
```

```json
{"queue_control": "pause"}
{"queue_control": "resume"}
{"queue_control": "stop"}
{"queue_control": "restart"}
```

Max 20 steps. Entering Queue with a loaded program starts it.
`auto_start` also starts after reboot.

### Hybrid — climate lock on relay 1, timed irrigation on relay 2

```json
{"mode": 3}
{
  "hybrid": {
    "logic": "AND",
    "automation": {
      "rules": [
        {"node": 1, "relay": 1, "temp": {"min": 18, "max": 26}}
      ]
    },
    "queue": {
      "loop": true,
      "steps": [{"relay": 2, "duration": 20}]
    }
  }
}
```

Automation can lock a relay ON. The queue drives relays it does not own.
Hybrid stale window is **60 s** (automation-only is 5 minutes).

---

## 13. Gateway BOOT button (GPIO 0)

Hold, then **release** in the window. Longer holds win.

| Hold | Action |
|---|---|
| ≥ 1 s (release before 5 s) | Start pairing |
| ≥ 5 s (release before 10 s) | Erase Wi-Fi, reboot |
| ≥ 10 s (release before 20 s) | Forget all sensors, reboot |
| ≥ 20 s | Factory reset |

Do not use 20 s unless you intend a full wipe.

---

## 14. MQTT cheat sheet

| Topic | Direction | Use |
|---|---|---|
| `{MAC}/command` | you → device | JSON commands |
| `{MAC}/status` | retained | Snapshot |
| `{MAC}/node/{id}/data` | publish | Temp / humidity, id 1–16 |
| `{MAC}/ota/status` | retained | OTA check / result |
| `{MAC}/event_log` | publish | Offline relay batches |

```json
{"pair": true}
{"link_test": "hub"}
{"link_test": "display"}
{"link_test": "all"}
{"link_test": "stats"}
{"ota_check": true}
{"ota_update": true}
{"event_log": "send"}
{"event_log": "clear"}
{"event_log": "count"}
{"event_log": "send_and_clear"}
```

While MQTT is down, relay edges append to SPIFFS `/event_log.jsonl` (max 5000
lines) and upload in batches of 20 on reconnect.

Compiled broker (change in `src/gateway/config.h` and rebuild): host
`82.112.230.108`, port `1883`, user `shunya`. Do not expose 1883 to the
public internet.

---

## 15. Field OTA (already-flashed boards)

Both S3s need a **full first flash** so dual OTA slots exist. Never send a
factory/merged binary over OTA.

**Gateway:** publish `{"ota_check": true}`, confirm `ota_available`, then
`{"ota_update": true}`. Relays turn off, inactive bank is written, board
reboots. Confirm the new version on USB/MQTT.

**HMI:** gateway must be on Wi-Fi and the HMI UART online. System tab →
**HMI CHECK** (gateway sends Wi-Fi over UART; credentials are not logged) →
**HMI UPDATE**. Keep power on until reboot. Confirm version and
`[HMI PERF]`.

If HMI check fails, the gateway logs
`HMI check failed at <stage> ram=<internal KiB>`:

| Stage | Meaning |
|---|---|
| `dns<n>` | IPv4 lookup failed |
| `tls<n>` | mbedTLS error (`tls-32512` = out of internal SRAM) |
| `http<n>` | HTTP status from the server |
| `ram-low` | Pre-flight SRAM check refused the handshake |

A broken OTA client cannot fix itself over the air. USB-flash with
`python3 scripts/shunya_build.py display upload`.

---

## 16. Common troubleshooting

| Symptom | What to do |
|---|---|
| Pairing countdown runs, sensor never joins | Watch the **hub** log for `pair open request`. No such line → UART is down (`test hub`). Sensor must be in range during the 120 s window. |
| Sensor joined Zigbee but no HMI row | Open pairing and wait for `joined node`. Confirm hub UART. |
| 17th sensor never appears | Capacity is 16. Forget an unused node first. |
| DHT paired, T/H always 0 | GPIO 6, `DHT_MODEL`, 4.7 kΩ pull-up, wait one sleep interval. |
| Battery always `--` | Wire GPIO 2 through a 2:1 divider, or send a third number on the H2 demo (`set 24.5 55 87`). |
| Sleep slider does nothing | Release the knob (send on release). MQTT `{"sensor_sleep": 60}` also works. |
| H2 demo ignores typing | Monitor that board at 115200, `set 24.5 55 87`, then pair. |
| H2 rejects a line | Two or three finite numbers, ranges `-40..80`, `0..100`, battery `0..100`. |
| HMI “link lost” | Cross 43↔44, common GND, UART selector, `test display`, Pico disconnected. |
| HMI black / dead touch | Exact Waveshare 7" model, PSRAM in USB log, flash `display_s3`. |
| Buttons ignored, data still updates | Command queue busy. Enable `diagnosticApp`; look for queue-full / latency. Wait or retry. |
| `test hub` fails | Cross S3 5→C6 4 and S3 4←C6 5, GND, 115200, current `zigbee_hub_c6`. |
| Serial monitor empty | Press reset. Close other monitors (`Get-Process pio`). Pass `-Port`. |
| BLE done, no MQTT | STA has an IP? Broker reachable on 1883? |
| Watchdog on IDLE0 / mqttTask | Broker slow or down. Do not raise the MQTT socket timeout. |
| `No OTA partition found` | Full `gateway_s3` flash, not app-only. |
| After Thread→Zigbee, hub reboot loop / NVS errors | Full Zigbee upload including `partitions_zigbee.csv`. |
| Old 4-node install looks empty | Schema v2 migrates up to four records once. If they were already corrupt, pair again. |
| HMI build `TypeError ... not 'NoneType'` | Arduino 2.x overwrote the HMI store. Delete `packages/framework-arduinoespressif32` there and rebuild with the display wrapper. |
| `No module named 'SCons.Tool.FortranCommon'` | Zigbee target was built with system `pio`. Delete `tool-scons*` in that store; use `scripts/shunya_build.py idf`. |
| `WindowsLongPathError` | Gateway stays on `espressif32@7.1.2`. HMI always via the display wrapper. |
| `intelhex` missing | `python -m pip install intelhex` into the interpreter that runs `pio`. |
| Package `WinError 3/5` or empty toolchain `bin` | Two builds ran together, or stores mixed. Delete the broken package folder; rebuild one env. |
| Compiler “not a full path” | Stale `pio` still installing. Kill leftovers, rebuild. |
| HMI OTA `tls-32512 ram=18k` | Internal SRAM too low for TLS. Need the PSRAM LVGL build, USB-flashed. |

---

## 17. Quick bring-up checklist

1. Wire GND + crossed UARTs. Do not share 5 V.
2. Flash gateway, then hub, then HMI, then one sensor. One USB at a time.
3. Provision Wi-Fi (`abcd1234`) if asked.
4. `test hub` and `test display` both pass.
5. Open pairing 120 s; wait for `application admission confirmed`.
6. Confirm MQTT `{MAC}/node/1/data` and the HMI Sensors page.
7. Programs → Manual → toggle a relay from the Relays tab.
8. Optional: load an automation rule and drive it with the H2 `set` command.

Hardware still required for mesh coverage claims: at least three powered FTD
boards, with one sensor out of direct collector range, so reports route through
another sensor.
)
