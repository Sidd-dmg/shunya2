# Four-mode validation issues

The firmware defects below were fixed in the gateway sources after this run. Flash `gateway_s3` to the ESP32-S3 before retesting. The H2 serial case remains blocked until COM16 is free.



Live run on 31 Oct 2026 against gateway firmware 4.2.1 on COM20 (MAC `68EE8F489678`, `192.168.1.10`) and the Zigbee hub on COM15 (PAN `0x66EB`, channel 12). Row-level results are in `tests/results/four-mode-validation-20261031-1946.csv`. The gateway was left in Manual mode with all seven relays off.

## Firmware defects

### 1. EEPROM commits fail, so reboot drops relay and sleep state

During the automated run the gateway serial log repeated:

```text
[EEPROM.cpp:197] commit(): error in write: ESP_ERR_NVS_NOT_ENOUGH_SPACE
```

Arduino `EEPROM` stores the whole 4096-byte image (`EEPROM_SIZE` in `src/gateway/config.h`) inside the 20 KB `nvs` partition (`0x5000` in `partitions_16mb_3mb_ota_spiffs.csv`). After the test's many commits, later writes did not stick.

Observed after an RTS reboot:

- Sleep had been set to 37 s. The first post-boot status reported `sensor_sleep_sec` 20, the default.
- Manual relay 5 was ON and confirmed in MQTT status. After reboot all seven relays were false.
- Boot logged `Loaded 0 nodes from EEPROM schema v2`. The hub later restored the two paired nodes over UART, so the dashboard can look recovered while the gateway's own EEPROM write path is failing.

Evidence: `tests/results/gateway-hmi-20261031-193727.serial.log`, `tests/results/gateway-hmi-20261031-193727.json` (`PERSIST recoverable reboot`).

Fix: the image is now 1024 bytes, which covers the used map through address 832. Boot copies an existing larger blob aside, erases it, and writes the smaller image before `EEPROM.begin`. A failed commit erases that blob once and retries. Manual relay saves also wait until the relay task has applied the command, so the stored state matches the output.

### 2. Replacing an automation rule leaves the old relay on

A rule for node 2 (40 °C) and relay 7, thresholds 30–35 °C, turned relay 7 on. Replacing that rule with thresholds 50–60 °C should have turned relay 7 off, because 40 °C is below the new minimum. Relay 7 stayed on until the 12 s wait expired.

`handleAutomationConfig` in `src/gateway/mqtt_commands.cpp` clears `lastState` to false and does not drive the previous relay off. `runAutomationMode` in `src/gateway/mode_engine.cpp` only calls `requestRelay` when the computed state differs from `lastState`. The new rule also evaluates to off, so the two values match and the output is never released.

Fix: before the rule table is replaced, the gateway records relays whose `lastState` is on and turns those relays off after the new table is installed. A new rule that still wants a relay on turns it back on, because its debounce timer starts at zero.

### 3. Hybrid keeps an automation-owned relay after the rule no longer wants it

Hybrid correctly ran relay 6 from the queue and relay 7 from automation at the same time. Replacing the automation rule so that 40 °C was below the new minimum should have released relay 7 and left relay 6 on. Relay 7 stayed on.

`runHybridMode` clears `hybridRelayStates` when a rule's `lastState` is false, and the later cleanup turns a relay off only while that flag is still true. Clearing the flag first skips the off command, so the same reconfiguration leaves the output energized.

Fix: that early clear is gone. While a rule owns a relay the flag stays set, and the existing cleanup turns the relay off when neither automation nor the queue still wants it. Rule replacement also uses the explicit release above.

### 4. Queue pause time still counts

A one-step queue turned relay 5 on for 2 s. Pause was accepted (`queue.paused` true). After 3 s, resume ended the queue immediately and turned relay 5 off.

`runQueueMode` returns while `queuePaused` is true, but it does not move `queueStepStartTime`. Elapsed time on resume is `millis() - queueStepStartTime`, so the pause is included in the step duration.

Fix: pause records its start time. Resume adds the paused interval to `queueStepStartTime`, so both queue and hybrid measure only running time. MQTT and the HMI use the same two functions.

## Test-harness defects

`tests/gateway_hmi_hil.py` still expects four-element relay arrays in several places, including the mixed-mask check around line 411 and the queue sequence / mode-exit checks. Live status from this gateway is seven booleans. Those cases were recorded as FAIL because the assertion timed out, while the serial log showed the seven-relay transitions the firmware was asked to make:

| Case | What the gateway actually did |
| --- | --- |
| Mixed mask | Status carried seven relay values; the checker waited for four |
| Queue sequence | Steps for relay 1, the delay, relays 2+3, then all off were all observed |
| Immediate mode exit | Serial logged relays 1–7 turning off |
| Graceful mode exit | Same stale four-value wait |

Those rows are harness failures. They are not evidence that the seven-relay queue runtime is broken.

Fix: `tests/gateway_hmi_hil.py` now builds seven-element expected relay vectors.

## Blocked

H2 serial input on COM16 was not run. Opening the port returned `PermissionError: Access is denied`, which means another program already had the monitor open. No `set` lines were sent, so parser limits, hub delivery, and hub-reboot recovery for typed values were not measured.

Host builds of the display codec, thread codec, hub slots, and EEPROM migration tests exited 1 without a compiler diagnostic in the captured output. That is an inconclusive toolchain result. The desktop HMI simulator self-test passed.

## What did pass

Manual control of relays 1–7, all-off, automation mode protection, automation and hybrid config storage, pairing open/close, sleep clamps (1→5, 4001→3600, 37→37), queue pause/resume/restart/stop, empty/20/25-step queue limits, malformed and oversized MQTT, a 21-command burst, event-log clear, hub link, and a 10-echo HMI UART soak (19–29 ms, CRC/framing stayed 0/1). Automation from a fresh node reading did energize relay 7, and hybrid did run queue relay 6 together with automation relay 7 before the release failure above.
