# Shunya ESP firmware: history, decisions and open work

Companion to [SKILL.md](SKILL.md) and [reference.md](reference.md). Read this before "fixing" something that looks odd; most of it has a reason.

Contents: 1 Evolution of the project, 2 Decisions and the reasons behind them, 3 Bugs found and how they were fixed, 4 Display link: what was analysed and what is implemented, 5 Validation evidence, 6 Pending work and next steps, 7 Environment gotchas on this machine, 8 Working agreements with the user.

## 1. Evolution of the project

1. Started as an ESP32-S3 gateway with four relays, an LVGL HMI and an OpenThread sensor network (`thread_*` files and tests, `THR-*` rows in the test CSV).
2. The sensor network moved to **Zigbee**: the C6 DevKitC-1 became a coordinator ("hub") with a UART link to the gateway, the C6-MINI-1 became the production sensor, an H2 board became a serial-driven bench sensor. The old Thread code is still in the tree but is not compiled.
3. Relays grew from four to **seven** (GPIO 15, 16, 9, 14, 21, 47, 48). The HMI protocol was bumped to v3, snapshot size is guarded by a static assert, and `EEPROM_SIZE` was set to 1024.
4. Sleep interval control, battery percent, LQI and a node trend chart were added across hub, gateway, HMI and MQTT.
5. The production sensor changed from a DHT22 (GPIO 6) to a **Sensirion SHT40 over bit-banged I2C** (SDA 22, SCL 23, user-specified pins). The DHT code remains only as an uncompiled `dht.c`.
6. Hub, sensor and H2 images were matched to their real **4 MB flash** modules (hub previously built for 8 MB). The hub needs its bootloader reflashed for this because the flash size is encoded in the bootloader header.
7. A factory folder generator and a one-device-at-a-time flasher were added, followed by a README rewrite.
8. Gateway/HMI UART robustness rules were implemented in source (see section 4) but not yet built or flashed at the time of writing.

## 2. Decisions and the reasons behind them

- **Never auto-delete a paired sensor.** A silent sensor can simply have a flat battery or be out of range. Deleting would drop its rules and history. Only explicit FORGET actions (HMI/MQTT/BOOT 10 s/20 s) remove nodes. The hub's LEFT event only marks a node inactive.
- **Bit-bang SHT40.** The user explicitly asked to keep the bit-bang driver (`sht40.c`). Do not swap it for the IDF I2C master driver.
- **One EEPROM blob in NVS, 1024 bytes.** A 4 KB blob does not fit the 20 KB NVS partition when it must be rewritten (old copy plus new copy). 832 bytes of the map are used.
- **Schema v3 node records (20 bytes)** store identity plus last temperature, humidity and battery so the HMI and MQTT show real values after a reboot. LQI is intentionally not stored: it is live link quality.
- **`lastSeen` is stamped at boot** for restored nodes so age starts at 0 s instead of showing as stale; the hub then replays the last reading.
- **Relay GPIO is written only by `relayTask`**; modes, MQTT and HMI request changes by queue. Manual-mode saves wait for the relay task to finish so the stored state matches the pins.
- **Mode changes can be "graceful"** (queue finishes its step) via MQTT `transition`.
- **UART kept for the HMI link** (see section 4). RS-485 needs transceivers, SPI needs pins that are not free, no extra hardware is available.
- **One toolchain per board family** because Arduino-ESP32 2.x, Arduino-ESP32 3.x (for the S3 panel and LVGL) and ESP-IDF 5.5 with Zigbee cannot share a platform. They live in separate PlatformIO core dirs (`C:\pio-idf`, `C:\pio-display`).
- **Factory flasher uses esptool directly** and sets `PYTHONUTF8=1` because PlatformIO's progress output crashed on the cp1252 console.
- **Hub NVS namespace stays `threadhub`** (a historic name from the Thread period) so it does not need to change.
- **LQI from the neighbor table.** The Zigbee attribute report does not carry LQI, so the hub reads `esp_zb_nwk_neighbor_info_t.lqi` for the sender's short address. Previously the field was always 0.

## 3. Bugs found and how they were fixed

| Symptom | Cause | Fix |
|---|---|---|
| LQI always 0 in HMI, MQTT and gateway | The hub never filled LQI because the attribute callback has no LQI | `neighbor_lqi()` in `zigbee_coord.c`; verified live (node 2 = 255) with `tests/results/check_lqi.py` |
| EEPROM commit failures (`NVS_NOT_ENOUGH_SPACE`) | 4 KB blob in 20 KB NVS | `EEPROM_SIZE 1024`, shrink-before-write in `persistence.cpp` |
| Node temperature/humidity lost after reboot | v2 schema stored only identity | v3 schema with values and battery, migration test in `tests/persistence_migration_test.cpp` |
| Relay state saved before the relay applied it | Save raced the relay queue | `relayWaitForIdle(200)` before manual saves |
| Hub image 8 MB header on a 4 MB module | Board definition default | `board_upload.flash_size`, `board_build.flash_size`, `maximum_size`, `sdkconfig.*` 4 MB; reflash bootloader |
| H2 demo console did not accept typed input | Console was UART, board uses USB Serial/JTAG | USB Serial/JTAG primary in both `sdkconfig.defaults.sensor_h2_demo` and `sdkconfig.zigbee_sensor_h2_demo`, `setvbuf(stdin, NULL, _IONBF, 0)` |
| First `build_factory.py` ran for about 23 h with no output | Tool call interrupted, build process left running | Kill leftovers, rerun; script now prechecks outputs before deleting `factory/` |
| `build_factory.py --skip-build` complained about the sensor `flasher_args.json` | Rebuilding the hub wiped the sibling IDF build dirs (shared `project.checksum`) | Rebuild sensor and demo, document in SKILL.md; precheck prevents wiping `factory/` first |
| `flash_boards.py` hub step failed with esptool 5 | esptool 5 syntax change (`write-flash`, pair arguments) | Factory flasher handles both; direct esptool call for manual flashing |
| PlatformIO upload of the hub stalled | `UnicodeEncodeError` in the progress bar on cp1252 | `PYTHONUTF8=1` and `PYTHONIOENCODING=utf-8` |
| Hub link step crashed once (exit 3221225477) | Toolchain crash on Windows | Rerun the same build |

## 4. Display link (gateway <-> HMI)

**Question:** can something more robust than UART be used, with no delay and no dropped signals?

**Analysis (conclusions to keep):**
- RS-485 is robust but needs a transceiver on each side. User cannot add hardware. Rejected.
- SPI would need MOSI/MISO/SCK/CS on four free pins across two boards. The four connected wires are GPIO 43, 44, 4, 5, but on the Waveshare board GPIO 4 is the touch interrupt and GPIO 5 is LCD enable, so SPI is not possible there.
- A UART at 115200 can carry about 11.5 KB/s. One snapshot is up to about 700 bytes (about 60 ms on the wire). The likely cause of perceived delay is snapshot traffic queued ahead of touch commands plus a blocking flush, not the baud rate. If more headroom is needed, raise the baud to 921600 on both sides (same wires, needs a matched change in both builds; wiring length and noise must be checked). Not done yet.

**Implemented in source (UART rules, "first we will try to apply the rules you mention to the uart"):**
1. Command first: pending HMI commands are processed before the periodic snapshot, which is deferred (forced after 2x interval); the mode task yields 1 ms instead of 50 ms while commands wait.
2. Immediate retransmit on CRC failure: new frame type `DISPLAY_MSG_LINK_RETRY = 0x0A`, 30 ms minimum between requests, both sides remember the last state-bearing frame (not heartbeats or retries) and resend it once.
3. `flush()` only for frames of 64 bytes or less.
4. 2048-byte TX buffers on gateway and HMI; the HMI drains one queued item per loop.
5. `displayHasPendingCommands()` exposed from `src/gateway/display_link.cpp`.

**Not yet done:** compile `gateway_s3` and `display_s3`, flash both together (protocol v3 is unchanged on the wire for old frame types, but both ends must carry the retry type to benefit), then measure latency and drops. Suggested measurement: run `tests/gateway_hmi_hil.py --phase automated`, watch `gateway_display_crc_errors` in the snapshot, and time touch -> ACK. Add a sequence-gap counter if drops still occur.

## 5. Validation evidence

- Four-mode validation (Manual, Automation, Queue, Hybrid) was run live on the bench with the H2 as sensor, plus reboot cases; results and a timestamped CSV are in `tests/results/` (gitignored) and the narrative is in `tests/four-mode-validation-issues.md` (its header still says H2 blocked / flash before retest, which is historical).
- H2 serial parser and hub -> gateway sensor-data path were validated. HMI touch cases were blocked at the time by link/switch state; C6 SHT40 cases were blocked by the electrical NOT_FOUND condition.
- LQI check: six typed `set` values on COM16, then MQTT status showed node 2 with LQI 255 (the check script reads credentials from `config.h` and does not print them).
- Hub 4 MB, sensor and demo images built; factory package generated (hub image 612,400 bytes) and manifest verified.
- Final safe state after tests: Manual, all relays off, sensor sleep 20 s.

## 6. Pending work and next steps

User-requested and not yet done:
1. Build, flash and test the UART robustness changes (section 4). Ask which ports to use; re-identify them first.

Known defects (do not fix unless asked, but mention if relevant):
- Automation rules EEPROM overlap at 8 or more enabled rules.
- HMI all-relays command bypasses the Manual-mode check.
- HMI queue stop does not persist `auto_start=false`.
- `hybridLogic` unused.
- SHT40 on the C6 board: electrical, check wiring and power before any more driver work.
- Hub UART pin discrepancy: `hub_config.h` says TX 16 / RX 17, README and earlier logs say TX 4 / RX 5 (section 1 of reference.md). Verify with the hub boot log and the actual wiring.
- Sensor entry in the factory manifest may report 8 MB flash from a stale `flasher_args.json`; configuration says 4 MB.
- `instructions.md`, `tests/four-mode-validation-issues.md` header and the PAIR/THR test rows are stale.
- HIL cases listed as untested: H2 malformed lines and CR/LF variants, hub reboot while a report is in flight, HMI touch after a long UART soak.

## 7. Environment gotchas on this machine

- Windows 11, PowerShell. `head`, `grep`, `sed` are not available; use the file tools or `Get-Content`/`Select-String`. PowerShell does not support `<<EOF` heredocs; write a script file and run it.
- `python` is the PlatformIO/system Python. The IDF store is `C:\pio-idf` and the HMI store is `C:\pio-display`.
- Long builds exceed a default tool-call timeout. Start them in the background and poll the log; do not start a second build while one runs.
- Serial ports: COM numbers change when boards are re-plugged. Native USB boards enumerate as VID `303A`. Always match the USB serial number to the board before flashing. Hold BOOT and tap reset only if the board is not entering download mode automatically.
- The HMI UART selector switch must be on the PC side to flash the HMI and on the gateway side in operation; "buttons do nothing but data updates" usually means the switch or TX wire.
- Large unrelated untracked files exist in the repo root (`factory.zip`, `factory (2).zip`, `src/idf_component.yml.orig`). They are not part of the project; do not commit them.

## 8. Working agreements with the user

- Prefer doing the work and verifying it on the bench over describing it. Show evidence (logs, MQTT status) rather than claims.
- Do not commit or push unless asked.
- Do not print credentials; secrets stay in `config.h`.
- Do not flash a port that is not positively identified.
- Keep descriptions of the system honest: say what is built, what is flashed, and what is only in source.
- The user answers questions about hardware (extra parts, wiring) directly; when a design needs hardware that is not available, propose the best option within the constraint rather than asking again.
- Documentation requests ("update the README to date") mean the document must match the code as it stands now, including versions and pin tables.
