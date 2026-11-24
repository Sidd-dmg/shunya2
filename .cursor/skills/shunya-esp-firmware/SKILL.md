---
name: shunya-esp-firmware
description: Explains the Shunya ESP climate/irrigation controller repo: ESP32-S3 gateway (7 relays, MQTT, four modes), Waveshare ESP32-S3 7-inch touch HMI, ESP32-C6 Zigbee coordinator hub, ESP32-C6-MINI-1 SHT40 sleepy sensor and ESP32-H2 serial demo sensor. Covers architecture, hardware pins, the two UART protocols, persistence, build and flash workflow, the factory folder, known pitfalls and open issues. Use when working in shunya_esp-main, or when the user mentions Shunya, the gateway, HMI, hub, SHT40 sensor, H2 demo, LQI, sensor sleep interval, relay modes, display UART, factory flash, or this repo's firmware.
---

# Shunya ESP firmware

Read this first. It tells you what the system is, which rules are non-negotiable, where code lives, and how to build, flash and test without damaging the rig. Detail lives in two companion files:

- [reference.md](reference.md): pin maps, both wire protocols byte by byte, EEPROM/NVS maps, MQTT topics and commands, mode semantics, timings, build environments.
- [project-history.md](project-history.md): why things are the way they are, bugs found and fixed, validation results, defects still open. Read it before "fixing" anything that looks odd.

Versions at the time of writing: gateway firmware `4.2.1` (`CURRENT_VERSION` in `src/gateway/config.h`), HMI `1.3.0` (`HMI_FIRMWARE_VERSION` in `src/display/display_config.h`). Check those files, not this text, for the current value.

## 1. What the product is

A climate and irrigation controller. Wireless Zigbee sensors measure temperature, humidity and battery. A gateway decides, using one of four operating modes, which of seven relays (pumps, valves, fans) to switch. A 7-inch touch screen shows state and sends commands. The gateway also talks MQTT to a cloud broker, provisions Wi-Fi over BLE, and updates itself over HTTP OTA.

There are five separate firmware images on five different boards. They are built by one PlatformIO project (`platformio.ini`) but with three incompatible toolchains.

| Role | Board | PlatformIO env | Source dir | Framework |
|---|---|---|---|---|
| Gateway (the brain) | 4D Systems GEN4 ESP32-S3 R8N16, 16 MB | `gateway_s3` | `src/gateway/` | Arduino 2.0.17 (espressif32@7.1.2) |
| HMI | Waveshare ESP32-S3-Touch-LCD-7 (800x480, GT911 touch) | `display_s3` | `src/display/` | Arduino 3.x (pioarduino) + LVGL 8.4 |
| Zigbee hub | ESP32-C6 DevKitC-1, **4 MB flash** | `zigbee_hub_c6` | `src/hub/` | ESP-IDF 5.5.5 (pioarduino) |
| Production sensor | ESP32-C6-MINI-1 on DevKitM-1 + Sensirion SHT40 | `zigbee_sensor_c6` | `src/sensor/` | ESP-IDF 5.5.5 |
| Bench sensor | ESP32-H2-DevKitM-1, 4 MB | `zigbee_sensor_h2_demo` | `src/sensor/` with `SENSOR_DEMO_SERIAL=1` | ESP-IDF 5.5.5 |

`src/common/` is shared: `display_protocol.*` (gateway<->HMI), `link_protocol.*` (gateway<->hub), `shunya_zb.h` (Zigbee cluster IDs, sleep limits, battery sentinel).

## 2. How the pieces talk

```
SHT40 C6 / H2 demo  --Zigbee (sleepy end device)-->  C6 hub (coordinator)
                                                        |  UART1 115200, 0xAA..0x55 frames, XOR checksum
                                                        v
Waveshare HMI  <--UART2 115200, A5 5A frames, CRC16, ACK-->  S3 gateway  --GPIO-->  relays 1-7
                                                        |
                                                      Wi-Fi: MQTT (plaintext 1883), NTP, HTTP OTA, BLE provisioning
```

Data path for one reading: sensor wakes, measures, reports temperature, humidity, battery to the coordinator (short address 0x0000, endpoint 1) -> hub `zb_action_handler` -> `handle_report` updates the slot and copies LQI from the Zigbee neighbor table -> `hub_uart_send_sensor` (23-byte `HUB_EVT_SENSOR`) -> gateway `hubRxTask` -> `sensorQueue` -> `sensorTask` in `mode_engine.cpp` updates `nodes[]`, saves to EEPROM at most every 5 minutes, publishes `{MAC}/node/{id}/data` -> automation rules read the same table -> next snapshot to the HMI.

Command path: HMI touch or MQTT JSON -> queue -> `modeTask` (the only place that mutates mode/relay/persistence state) -> `relayTask` (the only place that writes GPIO) -> status republished.

Identity: MQTT client id and topic prefix is the gateway station MAC without colons (`{MAC}`). A sensor is identified by its Zigbee IEEE address and mapped to a logical node id 1-16. Node ids are the same on MQTT, automation rules, EEPROM and HMI.

## 3. Rules that must not be broken

These come from explicit user instructions or from hardware facts that cost real debugging time.

**Process rules**
- Do not `git commit` or `git push` unless the user asks.
- Never print or copy the MQTT password. It is a compile-time constant in `src/gateway/config.h`. Tools read it from there; do not echo it into chat, logs, CSVs or docs.
- Never flash a port you have not identified. List ports with VID `303A` and check the USB serial number / MAC against the board. Intel AMT and Bluetooth COM ports (often COM3, COM12, COM13) are not boards. Several Espressif boards may be attached at once.
- Never run two firmware builds at the same time. The three toolchains install into shared package folders and corrupt each other.
- Do not hand-edit the generated EEZ/LVGL tree `src/display/ui/generated/` unless the task is about it; the UI behaviour lives in `ui/ui_controller.cpp`.

**Behaviour rules**
- **A paired sensor is never deleted automatically.** Only per-node FORGET, FORGET ALL, boot-button 10 s hold, or 20 s factory reset remove one. A Zigbee leave announcement, a silent sensor, or a missing hub LIST must not remove it. Age shown in the UI is informational.
- Keep the bit-bang SHT40 driver (`src/sensor/sht40.c`, SDA GPIO 22, SCL GPIO 23, address 0x44); the user explicitly asked for it. Do not swap in the IDF I2C master driver.
- Battery `0%` on the SHT40 board is an unconnected ADC pin (GPIO 2), not a bug. `0xFF` means unknown.
- Relay numbering: MQTT keys `relay1`..`relay7` and automation/queue rules are 1-based; the HMI wire protocol and arrays are 0-based. `relay_index 0xFF` means all.
- `control_states[]` in the gateway is legacy: only relays 1-4 live in `[0..3]`, `[4]` is mode. Never write relays 5-7 there.
- Do not erase the whole NVS partition on the gateway. Wi-Fi credentials live there.
- The display link must stay on GPIO 43/44 UART. GPIO 4 (touch interrupt) and GPIO 5 (LCD data enable) on the HMI are taken, so SPI between gateway and HMI is not possible on the connected wires. No extra hardware (no RS-485 transceiver) is available.

## 4. Repository map

```
platformio.ini                       five envs, platforms pinned explicitly
partitions_16mb_3mb_ota_spiffs.csv   gateway: NVS 20 KB, 2 x 3 MB OTA, SPIFFS
partitions_zigbee.csv                hub/sensor/H2: 4 MB-safe table
sdkconfig.defaults.hub_c6 | .sensor_c6 | .sensor_h2_demo   per-env IDF defaults
sdkconfig.zigbee_*                   generated per-env config (gitignored, but the ACTIVE one)
src/gateway/                         app_state, button, config.h, diagnostics, display_link, event_log,
                                     hub_link, link_test, main, mode_engine, mqtt_commands, mqtt_service,
                                     ota_service, persistence, relay_control, rtos, wifi_prov
src/display/                         main, display_link, display_state, hmi_ota, board/waveshare_panel,
                                     ui/ui_controller.cpp (all behaviour), ui/generated/ (EEZ output)
src/hub/                             main, hub_uart, slots, zigbee_coord   (thread_hub.c is dead code)
src/sensor/                          main, sht40, zb_sensor              (dht.c, thread_sensor.c are dead code)
src/common/                          display_protocol, link_protocol, shunya_zb.h
scripts/shunya_build.py              cross-platform build/upload/monitor/release/simulator wrapper
scripts/build_factory.py             builds all five images into factory/
scripts/factory_flash.py             copied to factory/flash.py
scripts/flash_boards.py              older four-board flasher (no H2 demo)
tests/                               host tests, HIL harness, CSV test matrix, validation write-up
web/mqtt-console/server.py           local web MQTT console (no auto-command)
hmi/, simulator/                     EEZ project, desktop LVGL simulator
README.md                            developer guide (updated to the Zigbee system)
instructions.md                      operator manual, PARTLY STALE (see project-history.md)
```

The CMake in `src/CMakeLists.txt` selects sources by role: ESP32-S3 target builds `gateway/*.cpp` + `common/*.c`; `SHUNYA_ZB_ROLE=sensor` builds `sensor/main.c`, `sht40.c`, `zb_sensor.c`; anything else is the hub. Files not listed there are not compiled.

## 5. Build, flash and factory workflow

Three stores, three commands. Always use the wrapper for anything except the gateway.

```bash
pio run -e gateway_s3                                          # gateway (system PlatformIO)
python scripts/shunya_build.py idf hub|sensor|demo build       # Zigbee boards, store C:\pio-idf
python scripts/shunya_build.py display build                   # HMI, store C:\pio-display
python scripts/shunya_build.py idf hub upload --port COM15
pio run -e gateway_s3 -t nobuild -t upload --upload-port COM20
```

Build outputs: `.pio/build/gateway_s3`, `.pio/build-idf/<env>`, `.pio/build-display/display_s3`.

**Factory package** (what the user asked for last):

```bash
python scripts/build_factory.py            # builds all five, writes factory/
python scripts/build_factory.py --skip-build   # package only; refuses if any output is missing
python factory/flash.py                    # asks for each board's port, one at a time, 'skip' allowed
python factory/flash.py --gateway COM20 --display COMx --hub COM15 --sensor COMy --demo skip --yes
```

`factory/` holds `<key>-bootloader.bin`, `-partitions.bin`, `-firmware.bin`, `boot_app0` for the two Arduino boards, `manifest.json` (offsets, chip, flash mode/freq/size, SHA-256) and `flash.py`. It is gitignored. `flash.py` uses esptool directly with `PYTHONUTF8=1` and refuses to write two devices to one port. Before the HMI step the UART switch must be toward the PC, and back toward the gateway afterwards.

Flash offsets: Arduino S3 boards 0x0 bootloader, 0x8000 partitions, 0xE000 boot_app0, 0x10000 app. IDF boards 0x0, 0x8000, 0x10000. A hub flash-size change (8 MB -> 4 MB) lives in the bootloader header, so flash the bootloader too, not only the app.

Build pitfalls seen in practice:
- A long IDF build can be silently abandoned if the tool call is interrupted. Check for leftover `pio`/`cmake` processes before rebuilding.
- Changing an IDF env's config can make PlatformIO erase **other** envs' output under `.pio/build-idf` (they share one `project.checksum`). After rebuilding the hub, the sensor and demo outputs vanished. Rebuild all three before `build_factory.py --skip-build`.
- The hub link step once crashed with exit `3221225477`; rerunning the same build succeeded.
- PlatformIO's esptool progress bar can crash on a cp1252 console and hang the upload. Flash with `PYTHONUTF8=1`, or use `factory/flash.py`.
- Active IDF config is `sdkconfig.<PIOENV>` in the repo root, regenerated only when that file is newer than `build.ninja`. Editing only `sdkconfig.defaults.*` does not change an existing env; edit the `sdkconfig.<env>` file too (this is how the 4 MB hub and the USB-console H2 demo were fixed).
- Host g++ from Python/PowerShell needs `C:\msys64\ucrt64\bin` on PATH or it exits 1 with empty output.

## 6. Where to make a change

| Task | Touch |
|---|---|
| New MQTT command | `src/gateway/mqtt_commands.cpp` (parse), `processMqttCommandPayload`; keep ownership in `modeTask` |
| New HMI command | add enum to `src/common/display_protocol.h` (bump `DISPLAY_PROTO_VERSION` only if the wire layout changes), handler in `processCommand` in `src/gateway/display_link.cpp`, sender in `ui_controller.cpp`; keep `static_assert(RELAY_COUNT == DISPLAY_RELAY_COUNT)` true |
| New field in HMI snapshot | `display_snapshot_t` (must stay <= 768 bytes), `fillSnapshot`, `ui_controller.cpp` |
| New hub message | `link_protocol.h` command, `hub_uart.c` (hub), `hub_link.cpp` (gateway); payload max 48 bytes |
| Change sensor interval behaviour | `hub/zigbee_coord.c` (`send_sleep_write`, `handle_report`), `sensor/zb_sensor.c` (`zb_attribute_handler`, `report_task`) |
| New persistent setting | `config.h` EEPROM map, `persistence.cpp`; check the map in reference.md for overlap first |
| Relay behaviour | `mode_engine.cpp` run*Mode functions; GPIO only in `relay_control.cpp` |

Always rebuild every target that includes `src/common/*` when you touch it, and flash gateway and HMI together after a display-protocol change.

## 7. Behaviours that look like bugs but are not

- **Sleepy end device.** The sensor reports every interval (default 20 s, range 5-3600) and listens only for about 800 ms after a report. A write sent between reports is missed, so the hub writes the sleep value when the node joins, when the gateway changes it, and again after the next temperature report from that node. A new interval can therefore take up to one old interval to apply. A sensor ignores an unchanged value.
- **H2 serial input.** A typed `set` line wakes the report task and normally reports immediately. If it arrives during the 800 ms window after a report, the notification is consumed there and the next report waits a full interval.
- **LQI is 0** for a paired node that has not transmitted since the hub booted. It is live data from the Zigbee neighbor table, not stored in EEPROM. A real value was observed as 255.
- **After a gateway reboot** `lastSeen` is stamped to boot time so the age starts at zero; the hub then resends the last stored reading.
- **Automation ignores stale samples** (older than 5 min; hybrid 60 s). That is intentional safety, not deletion.
- **Debounce.** Automation changes a relay only after 3 s unless it is the first change. Status republishes every 10 s unless a command forces it.
- **Relay save race.** Manual saves wait for `relayWaitForIdle(200)` so the stored state is what the relay task actually applied.
- **HMI "draws but buttons do nothing".** Display RX works, display TX or the UART switch is wrong. Check the switch position and the echo test (`test display`) before changing code.
- **Four-relay leftovers.** Several docs, `tests/system_test_cases.csv`, and old assertions refer to four relays. The firmware has seven.

## 8. Testing

- Gateway console commands (USB, 115200): `test hub`, `test display`, `test display-tx`, `test display-loop` (HMI unplugged, jumper GPIO43 to GPIO44), `test all`, `diagnosticApp [off|status]`. The same link tests can be triggered over MQTT with `{"link_test": "display-tx"}`.
- Host tests: `tests/persistence_migration_test.cpp` (EEPROM schema migration, built with g++ and `-Itests/stubs -Isrc/gateway -Isrc/common`), `tests/display_protocol_test.c`. `thread_*` tests belong to the retired Thread path.
- HIL: `python tests/gateway_hmi_hil.py --port COMx --phase automated|baseline|cleanup|physical|all`. Needs `tests/requirements.txt` (pyserial, paho-mqtt). It reads the broker settings from `config.h` and always tries to end in Manual mode with all relays off.
- Sensor-driven mode tests: type `set <temp> <humidity> [battery]` into the H2 demo monitor and watch MQTT `{MAC}/status` and `{MAC}/node/{id}/data`.
- Leave the rig safe: mode 0 (Manual), all relays off, sensor sleep 20 s.
- Results go in `tests/results/` (gitignored). `tests/four-mode-validation-issues.md` documents the four-mode validation and is partly historical.

## 9. Known open issues

Do not report these as new findings; do not fix unless asked.

1. **Automation EEPROM overlap.** Rules start at address 110 and take 23 bytes per enabled rule. Seven rules end near 272. Eight or more reach the queue state at 290; sixteen also overwrite hybrid (450), sleep (496) and node storage (500/512).
2. **HMI all-relays bypass.** `relay_index == 0xFF` in `DISPLAY_CMD_RELAY` skips the Manual-mode check, so the same frame can turn every relay on in other modes. The panel only uses it for all-off.
3. **HMI queue stop does not save `autoStart=false`** (MQTT stop does), so an autostart queue stopped from the HMI can restart after a reboot.
4. **SHT40 on the C6 sensor board.** A full bus scan with zero ACKs and both lines high means the chip sees only pull-ups: electrical (3.3 V, GND, SDA 22, SCL 23, power cycle). The driver already retries discovery on every failed sample. Do not keep rewriting it without a new log showing an ACK.
5. **`hybridLogic` is stored and reported but ignored** by `runHybridMode`; per-rule `logicOR` is what applies.
6. **Stale docs.** `instructions.md` still says gateway 4.0.1 / HMI 1.2.0 and lists the old hub UART wiring (`S3 5->C6 4`); the real wiring is gateway TX18/RX17 <-> hub RX5/TX4. The `THR-*` rows (Thread era), PAIR-004 and PAIR-005 in `tests/system_test_cases.csv` describe Thread or deferred-removal behaviour that no longer exists. `README.md` is the current document.
7. Untested edge cases are not defects: H2 malformed-line and CR/LF variants, hub reboot during a report, HMI touch after the UART soak.
