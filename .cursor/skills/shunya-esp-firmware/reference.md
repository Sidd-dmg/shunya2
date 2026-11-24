# Shunya ESP firmware: technical reference

Companion to [SKILL.md](SKILL.md). Every value here was read from the source or observed on the bench. When the source and this file disagree, the source wins; fix this file.

Contents: 1 Hardware and pins, 2 Gateway software structure, 3 Operating modes, 4 Persistence, 5 MQTT, 6 Hub UART protocol, 7 Display (HMI) protocol, 8 Zigbee, 9 Sensor firmware, 10 HMI firmware, 11 Build environments and partitions, 12 Bench rig, 13 Scripts and tests.

## 1. Hardware and pins

### Gateway: 4D Systems GEN4 ESP32-S3 R8N16 (16 MB flash, 8 MB PSRAM)

| Function | GPIO | Notes |
|---|---|---|
| Relay 1-7 | 15, 16, 9, 14, 21, 47, 48 | HIGH = on. `relayPins[]` in `relay_control.cpp`, mapped by `RELAY1_PIN..RELAY7_PIN` in `config.h` |
| Boot button | 0 | Hold: 1 s starts pairing, 5 s resets Wi-Fi, 10 s forgets all nodes, 20 s factory reset |
| HMI UART (Serial2) | RX 44, TX 43 | 115200 8N1. Cross-connected to HMI TX 43 / RX 44, common GND |
| Hub UART (Serial1) | RX 17, TX 18 | 115200 8N1, `HUB_UART_*` in `config.h`. Gateway TX18 goes to hub RX, gateway RX17 comes from hub TX |
| Console | USB, 115200 | `Serial` |

### C6 hub: ESP32-C6 DevKitC-1, **4 MB flash**
- Role: Zigbee coordinator, UART1 to the gateway at 115200, 16 node slots, NVS namespace `threadhub` (historic name kept for compatibility), key `slots`.
- UART pins come from `src/hub/hub_config.h` (`HUB_UART_TX_GPIO`, `HUB_UART_RX_GPIO`). **Discrepancy to resolve:** at the time of writing the header says TX 16 / RX 17, while `README.md`, the earlier hub boot logs (`UART GPIO RX5 TX4`) and git history before commit `a1b065e` say TX 4 / RX 5. GPIO 16/17 are also the default UART0 console pins on the C6 (`CONFIG_ESP_CONSOLE_UART_NUM=0`). Before wiring, flashing or documenting the hub link, read `hub_config.h`, check the hub boot log line `hub_main: ... UART GPIO RX%d TX%d`, and ask the user which pins are physically wired. Do not assume.
- Console: UART0 default with USB Serial/JTAG as secondary.

### Production sensor: ESP32-C6-MINI-1 (DevKitM-1 footprint) + Sensirion SHT40
- SHT40: SDA GPIO 22, SCL GPIO 23, address 0x44, bit-banged in `sht40.c` (do not swap for the IDF I2C driver).
- Button GPIO 9, hold 3 s = factory reset (leave network, wipe Zigbee storage).
- Battery sense GPIO 2 (ADC1_CH2, divider 2, 3000 mV empty, 4200 mV full). Unconnected = reports 0 %, `0xFF` = unknown.
- Avoid GPIO 8 (kit RGB LED) and GPIO 12/13 (USB). UART0 on an external adapter would be GPIO 16 TX / 17 RX.
- Default report interval 20 s. Model string `SHT40`, manufacturer `Shunya`.

### Bench sensor: ESP32-H2-DevKitM-1
- Built with `SENSOR_DEMO_SERIAL`, model `DEMO-SERIAL-H2`, report interval 5 s default.
- No sensor chip. Values are typed over the USB serial/JTAG console: `set <temp> <hum> [battery]`. Console is USB Serial/JTAG primary; stdin is unbuffered (`setvbuf(stdin, NULL, _IONBF, 0)`). Nothing is sent until paired.

### HMI: Waveshare ESP32-S3-Touch-LCD-7
- 800x480 RGB panel, GT911 touch, LVGL 8.4, 8 MB PSRAM, 16 MB flash.
- HMI UART: TX GPIO 43, RX GPIO 44 (the board's UART terminal), selected with the small UART switch.
- GPIO 4 is the touch interrupt and GPIO 5 is LCD data enable: not available, which is why SPI to the gateway is impossible on the four connected wires (43, 44, 4, 5).
- A UART selector switch routes the same pins to the PC USB-UART or to the gateway; it must face the gateway in operation and the PC when flashing.

## 2. Gateway software structure (`src/gateway/`)

FreeRTOS tasks (created in `main.cpp`):

| Task | Core | Prio | Stack | Role |
|---|---|---|---|---|
| mqttTask | 0 | 3 | 12288 | MQTT connect, subscribe, publish queue, status every 10 s |
| otaTask | 0 | 1 | 8192 | Hourly version check, install on request |
| hubTx / hubRx | 1 | 3 / 4 | 4096 / 6144 | UART to hub |
| sensorTask | 1 | 4 | 6144 | Applies hub events to `nodes[]`, EEPROM saves, publishes node data |
| displayRx | 1 | 3 | 6144 | HMI frames, commands, snapshots, heartbeat |
| relay | 1 | 4 | 4096 | The only task that writes relay GPIO |
| mode | 1 | 2 | 8192 | Mode engine, queue, automation, pairing timeout, display snapshot cadence |
| linkTest, diagnostics | 0 | 1 | 6144 / 4096 | Diagnostic commands, `diagnosticApp` |

Mutexes: nodes, relay, mqtt, spiffs, eeprom, displayUart, hubUart, state. Queues: sensor, control, mqttPublish, relay, cmd, hubTx, displayCmd. Pending touch commands are drained with 1 ms yield; otherwise the mode task sleeps 50 ms.

Key globals live in `app_state.cpp`: `nodes[]`, `nodeCount`, `relayStates[]`, `operationMode`, `autoRules[]`, `queueConfig`, `queueRunning`, `queuePaused`, `currentQueueStep`, `sensorSleepSec`, `pairingMode`, `deviceMAC`.

Wi-Fi: BLE provisioning with the Espressif provisioning app. Service name `Shunya <STA MAC>`, proof of possession in `PROV_POP_PIN` (do not print it). Credentials are stored by the Wi-Fi stack in NVS. NTP `pool.ntp.org`, UTC+5:30, resync hourly. OTA over HTTPS from raw GitHub URLs in `config.h` (`OTA_URL`, `VERSION_URL`); `markFirmwareValid()` runs at the end of setup.

## 3. Operating modes

Constants: `MODE_MANUAL 0`, `MODE_AUTOMATION 1`, `MODE_QUEUE 2`, `MODE_HYBRID 3`. Persisted at EEPROM 100. All mode transitions are done in `modeTask`; MQTT accepts `"transition":"graceful"` to let a running queue finish its step first.

- **Manual.** Relays follow commands from MQTT (`relay1..7`, `all`) and HMI. Saved relay states are restored on boot (EEPROM 101..107).
- **Automation.** Up to 16 rules. Each rule: node id 1-16, relay 1-7, temperature band (min/max), humidity band, either or both, AND/OR combination (`logicOR`). Hysteresis: above max -> on, below min -> off, in between -> keep the last state. A rule is ignored if the node is inactive or its last sample is older than 5 min (300000 ms). A relay changes at most every 3 s (first change immediate). Relays are all forced off on boot in this mode.
- **Queue.** Up to 20 steps, each a relay bitmask and a duration in seconds (mask 0 = pure delay), `loop` flag, `auto_start` flag. A step switches its relays on, waits, switches them off, moves on. At the end: loop, or stop and all relays off. Control: pause / resume / stop / restart. Pause shifts step timing so the remaining time is preserved. Boots running if auto_start is set.
- **Hybrid.** Queue and automation run together. Relays currently held on by an automation rule (rule `lastState` on) are "owned" by automation: queue steps skip them and they are never switched off by the queue. The queue drives the other relays. Freshness for hybrid is 60 s (not 5 min). `hybridLogic` (stored/reported as `hybrid.logic`) is not used by the engine.
- Mode-change safety: leaving automation or hybrid releases the relays that automation was holding (`automationReleaseMask`/`applyAutomationRelease`).

## 4. Persistence

### EEPROM (Arduino EEPROM emulation, one NVS blob opened with `nvs_open("eeprom", ...)` in `persistence.cpp`, size 1024)
NVS partition is 20 KB (0x5000), so the image cannot grow to 4 KB; a bigger image makes commits fail with `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. `persistence.cpp` shrinks/rewrites carefully.

| Address | Size | Content |
|---|---|---|
| 0..~176 | | Legacy v1 node records (11 bytes each) kept only for one-time migration |
| 100 | 1 | Operation mode |
| 101..107 | 7 | Manual relay states |
| 110 | 23 per enabled rule | Automation rules (16 max) |
| 290 | | Queue state |
| 300 | | Queue config (up to 20 steps) |
| 450 | | Hybrid config |
| 496 | 2 | Sensor sleep seconds |
| 500 | 4+ | Node schema magic `0x534E4F44` ("SNOD") and version (3) |
| 512 | 20 per node | Node records v3 (up to 16 -> ends at 832): id, 8-byte IEEE, short address, battery, temperature, humidity. LQI is **not** stored |

Overlap warning: rules beyond about 8 enabled run into the queue state at 290. See Known open issues in SKILL.md.

Save cadence: node readings are saved at most every 5 minutes and membership changes are saved when they happen; migration from v1/v2 to v3 is covered by `tests/persistence_migration_test.cpp`.

### Hub NVS (`threadhub`, key `slots`)
16 slots (`hub_slot_t`): `used`, `node_id`, IEEE, short address, `lqi`, `battery_pct`, `last_temp`, `last_hum`. The coordinator also stores the global sleep interval under its own `sleep` key. Slots survive reboot, so the gateway can ask `LIST_NODES` and get the last readings.

### Event log
SPIFFS `/event_log.jsonl`, up to 5000 entries, uploaded in batches of 20 on the `{MAC}/event_log` topic, cleared by command.

## 5. MQTT

Broker `82.112.230.108:1883` (plaintext), user `shunya`; the password is in `src/gateway/config.h`. Never print it.

| Topic | Direction | Content |
|---|---|---|
| `{MAC}/command` | in | JSON commands, payload max 3072 bytes |
| `{MAC}/status` | out, retained | Full state every 10 s and after commands |
| `{MAC}/node/{id}/data` | out | One sensor sample |
| `{MAC}/event_log` | out | Event log batches |

`{MAC}` = STA MAC without colons, upper-case (for the bench gateway `68EE8F489678`).

Status fields: `mode`, `mode_name`, `timestamp`, `uptime_ms`, `wifi_connected`, `wifi_rssi`, `wifi_channel`, `wifi_ssid`, `ip_address`, `mqtt_connected`, `pairing_mode`, `paired_nodes`, `sensor_sleep_sec`, `relays[7]`, `nodes[]` (`id`, `active`, `temperature`, `humidity`, `last_seen_ms`, `lqi`, `battery`, `short_addr`, `ieee`), `queue{...}` (queue/hybrid modes), `automation_rules[]` and `automation_rule_count` (automation/hybrid), `hybrid{logic, automation_active, queue_active}`, `event_log{pending_count,max_entries,topic}`, `ota_available`, `firmware_version`.

Node data fields: `node_id`, `temperature`, `humidity`, `battery` (omitted if unknown), `sleep_sec`, `timestamp`.

Command keys (any can be combined in one JSON object): `relay1`..`relay7` (bool/int), `all` (bool), `mode` (0-3), `transition` (`graceful`), `auto_save`, `automation{clear, rules[{node, relay, temp{min,max}, humidity{min,max}, logic}]}`, `queue{loop, auto_start, steps[{relays[], relay, duration, delay}]}`, `queue_control` (`pause|resume|stop|restart`), `hybrid{logic, automation, queue}`, `event_log` (`count|send|clear`; `count` replies with a small status JSON), `ota_update`, `ota_check`, `pair` (bool: true opens the 120 s window, false closes it), `sensor_sleep` (5-3600 s), `link_test` (`hub|display|display-tx|display-loop|all|stats`). Keys the parser does not know are ignored.

## 6. Hub UART protocol (gateway <-> C6 hub), `src/common/link_protocol.h`

Frame: `0xAA | cmd | len | payload[len<=48] | checksum | 0x55`. `checksum` = XOR of `cmd`, `len` and payload bytes. Max frame 53 bytes.

Gateway -> hub: `0x10 PING`, `0x11 GET_STATUS`, `0x12 PAIR_OPEN` (payload seconds), `0x13 PAIR_CLOSE`, `0x14 FORGET_NODE` (node id), `0x15 FORGET_ALL`, `0x16 LIST_NODES`, `0x17 ECHO`, `0x18 SET_SLEEP` (uint16 LE seconds).

Hub -> gateway: `0x20 SENSOR`, `0x21 JOINED`, `0x22 LEFT`, `0x23 PAIR_STATE` (open, remaining uint16), `0x24 STATUS` (pan id uint16, channel, node count, uptime ms uint32), `0x25 ACK`, `0x26 NACK`, `0x27 NODE_INFO` (one per slot in answer to LIST), `0x28 ECHO`.

Payload layouts (little endian):
- Node event (JOINED, LEFT): `node_id(1) ieee(8) short_addr(2) lqi(1)` = 12 bytes.
- SENSOR (23 bytes): `node_id` [0], IEEE [1..8], short address [9..10], `float temperature` [11..14], `float humidity` [15..18], `lqi` [19], `battery` [20], sleep seconds uint16 [21..22]. Note the LQI offset differs from the other messages.
- NODE_INFO (21 bytes): `node_id` [0], IEEE [1..8], short address [9..10], `lqi` [11], `float temperature` [12..15], `float humidity` [16..19], `battery` [20].
- STATUS (8 bytes): PAN id uint16, channel, node count, uptime ms uint32. PAIR_STATE (3 bytes): open flag, remaining seconds uint16.

Gateway behaviour on JOINED/LEFT: JOINED adds or refreshes a node. LEFT only marks the node inactive; it never deletes it. A `LIST_NODES` reply is used to resync, not to remove nodes. `0xFF` battery = unknown.

## 7. Display (HMI) protocol v3, `src/common/display_protocol.{h,c}`

Frame: `A5 5A | version(1)=3 | type(1) | flags(1) | reserved(1) | sequence(2 LE) | length(2 LE) | payload | CRC16(2 LE)`. Header = 10 bytes. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over every byte after the magic up to the end of payload. Max payload 768 bytes (`display_snapshot_t` is checked with a static assert).

Timings: heartbeat 1000 ms each way; the link is considered lost after 12000 ms of silence; the gateway sends a full snapshot every 5000 ms and immediately after any state-changing command.

Message types: `0x01 HELLO`, `0x02 SNAPSHOT_REQUEST`, `0x03 SNAPSHOT`, `0x04 HEARTBEAT`, `0x05 ACK` (request sequence, status, command), `0x06 ECHO`, `0x07 WIFI_CREDENTIALS`, `0x08 HMI_OTA_STATUS`, `0x09 WIFI_PROV`, `0x0A LINK_RETRY`.
Commands (HMI -> gateway): `0x20 RELAY` (index 0-6 or 0xFF=all, state), `0x21 MODE`, `0x22 PAIRING`, `0x23 FORGET_NODE`, `0x24 FORGET_ALL`, `0x25 QUEUE_CONFIG`, `0x26 QUEUE_CONTROL` (0 pause, 1 resume, 2 stop, 3 restart), `0x27 AUTOMATION_CONFIG`, `0x28 HYBRID_CONFIG`, `0x29 OTA`, `0x2A EVENT_LOG`, `0x2B WIFI_RESET`, `0x2C LINK_TEST`, `0x2D HMI_OTA`, `0x2E SENSOR_SLEEP`, `0x2F WIFI_PROV`.
ACK status: 0 OK, 1 bad length, 2 bad value, 3 wrong mode, 4 busy, 5 not found, 6 internal.

Snapshot contents (`display_snapshot_t`): uptime, MAC, IP, firmware string, Wi-Fi state/RSSI, MQTT state, OTA availability, operation mode, relay mask, pairing state and countdown, 16 nodes (`id`, IEEE, short address, temperature x100, humidity x100, LQI, age ms, active, battery), queue state and 20 steps, 16 rules, hybrid flags, hub PAN/channel/node count/online, event-log pending, link counters (tx, rx, CRC errors), sensor sleep seconds.

Link robustness rules implemented in v1.3.0 / 4.2.1 (UART kept; not yet built or run on hardware at the time of writing, see project-history.md):
1. **Command first.** The gateway handles pending touch commands before the periodic snapshot; a snapshot is deferred while commands wait (forced after 2x the interval).
2. **Immediate retransmit on corruption.** When a receiver's CRC error counter rises it sends `LINK_RETRY` (min spacing 30 ms). The sender resends its last state-bearing frame once, flagged as resent. `LINK_RETRY` and heartbeats are never remembered or resent.
3. **Flush only small frames.** `Serial.flush()` runs only for frames of 64 bytes or less, so a big snapshot does not stall the task for 80 ms.
4. **Bigger TX buffers (2048)** on both sides and the HMI drains one service item per loop.
5. Every command carries a sequence and is acknowledged with `DISPLAY_MSG_ACK` (request sequence, status, command).

## 8. Zigbee

- Coordinator on the C6 hub (ESP-IDF Zigbee/ZBOSS, `esp_zb_*`). Channel and PAN chosen at network formation (bench: channel 12, PAN 0x66EB). Permit join opens for 120 s from BOOT, HMI or MQTT.
- Endpoint 1, Home Automation. Standard clusters: temperature measurement `0x0402` and relative humidity `0x0405` (values x100). Manufacturer cluster `0xFC00` (manufacturer code `0x131B`): attribute `0x0000` sleep seconds (read/write), `0x0001` battery percent (read, reportable).
- The sensor reports every `sleep_sec` (5-3600 s, default 20 s; the H2 demo starts at 5 s) and listens only for about 800 ms after each report. The coordinator writes attribute 0x0000 in three situations: when a sensor joins, when the gateway sets a new interval (to every slot), and after the next temperature report from a node (a write sent between reports is missed). The sensor persists the value.
- Missing reports are not errors.
- **LQI** is read in `zigbee_coord.c` via `neighbor_lqi(short_addr)`: it walks the ZBOSS neighbor table with `esp_zb_nwk_get_next_neighbor` and returns `esp_zb_nwk_neighbor_info_t.lqi` (0-255; the standard attribute report carries no LQI). It stays 0 until the neighbor has exchanged traffic. Observed: node 2 = 255 at short range.
- Sensor identity: IEEE address, 8 bytes. The H2's IEEE is its USB MAC reversed with `FFFE` inserted (e.g. MAC `48:31:B7:C0:71:8C` -> IEEE `8C71C0FEFFB73148`).
- Joining: `esp_zb_bdb_open_network(duration)` on PAIR_OPEN (default 120 s), `esp_zb_bdb_open_network(0)` on PAIR_CLOSE. A join outside the window is ignored (`ignored join outside permit-join window`). On join the hub assigns the lowest free node id (`slots_alloc`), stores the slot in NVS, sends the sleep write and JOINED. A device announcing "leave" yields LEFT, not a delete. FORGET_NODE / FORGET_ALL free slots (`slots_free_index`, `slots_clear_all`).
- Zigbee storage partitions `zb_storage` (0xF1000, 16 KB) and `zb_fct` (0xF5000, 1 KB) are in the shared 4 MB table.

## 9. Sensor firmware (`src/sensor/`)

- `main.c`: init NVS, start Zigbee, button task. `zb_sensor.c`: Zigbee end device with `keep_alive` 3000 ms. `report_task`: waits 3 s after boot, then loops: measure (3 attempts, 50 ms apart), update the temperature / humidity / battery / sleep attributes, send three attribute reports, wait 800 ms (window for the hub's write), then wait `sleep_sec` (a task-notification wait; the code does not call MCU deep sleep). `zb_attribute_handler` receives the hub's sleep write, clamps it to 5-3600, ignores an unchanged value, stores it in the sensor's own NVS key `sleep`, and wakes the report task. Reports are sent by `esp_zb_zcl_report_attr_cmd_req` to the coordinator; no binding exists, delivery is by short address 0x0000 endpoint 1.
- `sht40.c`: bit-bang I2C using `esp_rom_delay_us`, measure command 0xFD (high precision), CRC-8 (poly 0x31, init 0xFF) on both words, `scan_bus()` when the device does not acknowledge, rediscovery on each failed sample. `ESP_ERR_NOT_FOUND` means the chip never acknowledged (electrical problem). A failed read logs `SHT40 read failed` and no report is sent for that cycle.
- Demo build: a typed `set 27.5 42.0 70` stores the values and notifies `report_task`, which normally reports immediately. If the notification arrives during the 800 ms post-report window it is consumed there and the next report waits a full interval.
- Dead code not compiled: `dht.c`, `thread_sensor.c`.

## 10. HMI firmware (`src/display/`)

- Arduino 3.x on ESP32-S3, LVGL 8.4, UI generated by EEZ Studio (`hmi/shunya-hmi.eez-project`, output in `ui/generated/`), behaviour in `ui_controller.cpp` (41 KB).
- Screens (see the README HMI section and the PNG exports in `hmi/` for the current layout): dashboard, relays, sensors (paged list; tapping a row opens a node trend), programs (automation rule table, queue step table, hybrid), and a system screen with Wi-Fi, OTA, HMI OTA, link test, sensor sleep, pairing and forget actions.
- `display_link.cpp`: UART2, parse frames, queue commands with sequence, ACK handling, heartbeat, retransmit logic, link-lost banner. `display_state.cpp`: snapshot cache and age. `hmi_ota.cpp`: HMI self-update from GitHub (`HMI_VERSION_URL`, `HMI_FIRMWARE_URL`) over Wi-Fi credentials passed by the gateway. `board/waveshare_panel.cpp`: panel, backlight, touch init (the GPIO 5 LCD enable and GPIO 4 touch IRQ lines are in here).
- Simulator: `simulator/` compiles the same `ui_controller.cpp` with a mock gateway; `scripts/hmi-simulator.ps1|.sh`.

## 11. Build environments and partitions

| Env | Platform | Flash | Partition table | sdkconfig |
|---|---|---|---|---|
| `gateway_s3` | espressif32@7.1.2 | 16 MB qio | `partitions_16mb_3mb_ota_spiffs.csv` (NVS 0x9000 20 KB, otadata 0xE000, app0 0x10000 3 MB, app1 0x310000 3 MB, SPIFFS 0x610000 ~10 MB) | none (Arduino) |
| `display_s3` | pioarduino (Arduino 3.x) | 16 MB | board default | none |
| `zigbee_hub_c6` | pioarduino ESP-IDF 5.5.5 | **4 MB** (`board_upload.flash_size`, `board_build.flash_size`, `maximum_size 4194304`) | `partitions_zigbee.csv` (NVS 0x9000 24 KB, phy 0xF000, factory 0x10000 0xE1000 = 900 KB, zb_storage 0xF1000, zb_fct 0xF5000) | `sdkconfig.zigbee_hub_c6` from `sdkconfig.defaults.hub_c6` |
| `zigbee_sensor_c6` | same | 4 MB | `partitions_zigbee.csv` | `sdkconfig.zigbee_sensor_c6` from `.defaults.sensor_c6` |
| `zigbee_sensor_h2_demo` | same, board esp32-h2-devkitm-1 | 4 MB | `partitions_zigbee.csv` | `sdkconfig.zigbee_sensor_h2_demo` from `.defaults.sensor_h2_demo` |

The shipped hub image is about 612 KB, so it fits the 900 KB app slot with room. Factory manifest note: the sensor entry may show an `8MB` flash size read from a stale `flasher_args.json`, while the config says 4 MB; check `build/flasher_args.json` for the sensor env if flashing.

Tool stores: `C:\pio-idf` (Zigbee), `C:\pio-display` (HMI), the normal PlatformIO home for the gateway. Python wrapper `scripts/shunya_build.py` sets `PLATFORMIO_CORE_DIR`, build dirs and `PYTHONUTF8`. It supports `idf <hub|sensor|demo> <build|upload|monitor|clean>`, `display <...>`, release packaging, and `simulator`.

Release binaries go to `dist/`. `factory/` is the all-in-one flashing package (SKILL.md section 5).

## 12. Bench rig (last confirmed)

| Device | Port | Identifier |
|---|---|---|
| Gateway | COM20 | USB serial `68:EE:8F:48:96:78`, MAC `68EE8F489678`, topic prefix `68EE8F489678`, firmware 4.2.1 |
| Hub | COM15 | USB serial `40:4C:CA:50:90:80`, PAN 0x66EB, channel 12 |
| H2 demo | COM16 | USB serial `48:31:B7:C0:71:8C`, IEEE `8C71C0FEFFB73148`, node id 2 |
| Not boards | COM3, COM12, COM13 | Intel AMT SOL and Bluetooth |

Ports change when cables move. Re-identify by VID `303A` and serial before any flash. Node id 1 on the bench was a second paired sensor that often reports 0 / 0.

## 13. Scripts and tests

- `scripts/shunya_build.py`: wrapper described above.
- `scripts/build_factory.py [--skip-build]`: build + package. Prechecks the five required outputs before touching `factory/`.
- `scripts/factory_flash.py` -> `factory/flash.py`: one-device-at-a-time flasher, options `--gateway --display --hub --sensor --demo <port|skip>`, `--yes`, uses esptool (searches PATH, `C:\pio-idf\penv\Scripts`, falls back to `python -m esptool`), refuses to reuse a port. esptool 5 uses `write-flash`; esptool 4 uses `write_flash`.
- `scripts/flash_boards.py`: older four-board flasher; its hub step breaks on esptool 5 ("Must be pairs of an address and the binary filename"). Prefer the factory flasher.
- `tests/gateway_hmi_hil.py`: HIL phases `automated`, `baseline`, `cleanup`, `physical`, `all`; writes CSV and a log under `tests/results/`; drives MQTT and serial.
- `tests/system_test_cases.csv`: the written test matrix (partly Thread-era rows).
- `tests/display_protocol_test.c`, `tests/persistence_migration_test.cpp`, `tests/thread_slots_test.c`, `tests/thread_codec_test.c`: host tests with stubs in `tests/stubs`.
- `web/mqtt-console/server.py`: local web console for MQTT status and commands.
