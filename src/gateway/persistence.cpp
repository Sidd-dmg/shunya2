#include "persistence.h"
#include "hub_link.h"
#include "app_state.h"
#include "rtos.h"
#include <EEPROM.h>
#include <Arduino.h>
#include <cstring>

struct __attribute__((packed)) NodeSchemaMarker {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t recordsCrc;
};

static uint16_t nodeRecordsCrc(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFFu;
    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void readNodeRecord(int address, NodeInfo &node) {
    node.nodeId = EEPROM.read(address);
    for (uint8_t j = 0; j < 8; ++j) {
        node.ieee[j] = EEPROM.read(address + 1 + j);
    }
    node.shortAddr = (uint16_t)EEPROM.read(address + 9) |
                     ((uint16_t)EEPROM.read(address + 10) << 8);
    node.active = true;
    node.lqi = 0;
    node.lastTemperature = 0;
    node.lastHumidity = 0;
    node.lastSeen = 0;
    node.batteryPercent = BATTERY_PCT_UNKNOWN;
}

void persistenceBegin(void) {
    EEPROM.begin(EEPROM_SIZE);
}

void initializeAutomationRules(void) {
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        autoRules[i].nodeId = 0;
        autoRules[i].relayId = 0;
        autoRules[i].tempMin = 20.0;
        autoRules[i].tempMax = 30.0;
        autoRules[i].humMin = 30.0;
        autoRules[i].humMax = 70.0;
        autoRules[i].enabled = false;
        autoRules[i].lastState = false;
        autoRules[i].useTempControl = false;
        autoRules[i].useHumControl = false;
        autoRules[i].logicOR = true;
        autoRules[i].lastStateChange = 0;
    }
}

void initializeQueueConfig(void) {
    queueConfig.stepCount = 0;
    queueConfig.loop = false;
    queueConfig.autoStart = false;
    memset(queueConfig.steps, 0, sizeof(queueConfig.steps));
}

void loadNodesFromEEPROM(void) {
    NodeSchemaMarker marker{};
    EEPROM.get(EEPROM_NODES_SCHEMA_ADDR, marker);
    uint8_t records[MAX_NODES * EEPROM_NODE_RECORD_SIZE];
    for (size_t i = 0; i < sizeof(records); ++i)
        records[i] = EEPROM.read(EEPROM_NODES_V2_START_ADDR + (int)i);

    const bool schemaValid =
        marker.magic == EEPROM_NODES_SCHEMA_MAGIC &&
        marker.version == EEPROM_NODES_SCHEMA_VERSION &&
        marker.count <= MAX_NODES &&
        marker.recordsCrc == nodeRecordsCrc(records, sizeof(records));
    if (schemaValid) {
        nodeCount = marker.count;
        for (uint8_t i = 0; i < nodeCount; ++i)
            readNodeRecord(EEPROM_NODES_V2_START_ADDR +
                               i * EEPROM_NODE_RECORD_SIZE,
                           nodes[i]);
        Serial.printf("Loaded %u nodes from EEPROM schema v%u\n",
                      nodeCount, marker.version);
        return;
    }

    /* A recognized v2 marker with an invalid count/CRC is a torn or corrupt
     * v2 write, not permission to resurrect stale legacy records. */
    if (marker.magic == EEPROM_NODES_SCHEMA_MAGIC &&
        marker.version == EEPROM_NODES_SCHEMA_VERSION) {
        nodeCount = 0;
        Serial.println("Node schema v2 corrupt; resetting node table");
        saveNodesToEEPROM();
        return;
    }

    const uint8_t legacyCount = EEPROM.read(EEPROM_NODE_COUNT_ADDR);
    nodeCount = legacyCount <= 4 ? legacyCount : 0;
    for (uint8_t i = 0; i < nodeCount; ++i)
        readNodeRecord(EEPROM_NODES_START_ADDR + i * EEPROM_NODE_RECORD_SIZE,
                       nodes[i]);
    Serial.printf("Migrating %u legacy EEPROM nodes to schema v%u\n",
                  nodeCount, EEPROM_NODES_SCHEMA_VERSION);
    saveNodesToEEPROM();
    /* Migration is one-way. Clearing the old count prevents an erased v2
     * marker from reviving records that the user later forgot. */
    EEPROM.write(EEPROM_NODE_COUNT_ADDR, 0);
    EEPROM.commit();
}

void saveNodesToEEPROM(void) {
    if (eepromMutex && xSemaphoreTake(eepromMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    uint8_t records[MAX_NODES * EEPROM_NODE_RECORD_SIZE] = {};
    const uint8_t savedCount = nodeCount <= MAX_NODES ? nodeCount : MAX_NODES;
    for (uint8_t i = 0; i < savedCount; ++i) {
        const size_t offset = i * EEPROM_NODE_RECORD_SIZE;
        records[offset] = nodes[i].nodeId;
        for (uint8_t j = 0; j < 8; ++j) {
            records[offset + 1 + j] = nodes[i].ieee[j];
        }
        records[offset + 9] = (uint8_t)(nodes[i].shortAddr & 0xFF);
        records[offset + 10] = (uint8_t)(nodes[i].shortAddr >> 8);
    }
    for (size_t i = 0; i < sizeof(records); ++i)
        EEPROM.write(EEPROM_NODES_V2_START_ADDR + (int)i, records[i]);
    const NodeSchemaMarker marker = {
        EEPROM_NODES_SCHEMA_MAGIC, EEPROM_NODES_SCHEMA_VERSION, savedCount,
        nodeRecordsCrc(records, sizeof(records))
    };
    EEPROM.put(EEPROM_NODES_SCHEMA_ADDR, marker);
    EEPROM.commit();
    if (eepromMutex) {
        xSemaphoreGive(eepromMutex);
    }
    Serial.println("Nodes saved to EEPROM");
}

void saveManualModeConfig(void) {
    for (int i = 0; i < 4; i++) {
        EEPROM.write(EEPROM_MANUAL_RELAYS_ADDR + i, relayStates[i] ? 1 : 0);
    }
    EEPROM.commit();
}

void saveModeConfig(void) {
    if (eepromMutex && xSemaphoreTake(eepromMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    EEPROM.write(EEPROM_MODE_ADDR, operationMode);
    EEPROM.commit();
    if (eepromMutex) {
        xSemaphoreGive(eepromMutex);
    }
}

void loadManualModeConfig(void) {
    for (int i = 0; i < 4; i++) {
        uint8_t state = EEPROM.read(EEPROM_MANUAL_RELAYS_ADDR + i);
        relayStates[i] = (state == 1);
        digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW);
        control_states[i] = relayStates[i] ? 1 : 0;
    }
}

void saveAutomationConfig(void) {
    int addr = EEPROM_AUTO_RULES_ADDR;
    uint8_t activeRules = 0;
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        if (autoRules[i].enabled) {
            activeRules++;
        }
    }
    EEPROM.write(addr++, activeRules);
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        if (!autoRules[i].enabled) {
            continue;
        }
        EEPROM.write(addr++, autoRules[i].nodeId);
        EEPROM.write(addr++, autoRules[i].relayId);
        EEPROM.write(addr++, autoRules[i].enabled ? 1 : 0);
        EEPROM.write(addr++, autoRules[i].useTempControl ? 1 : 0);
        EEPROM.write(addr++, autoRules[i].useHumControl ? 1 : 0);
        EEPROM.write(addr++, autoRules[i].logicOR ? 1 : 0);
        EEPROM.put(addr, autoRules[i].tempMin);
        addr += sizeof(float);
        EEPROM.put(addr, autoRules[i].tempMax);
        addr += sizeof(float);
        EEPROM.put(addr, autoRules[i].humMin);
        addr += sizeof(float);
        EEPROM.put(addr, autoRules[i].humMax);
        addr += sizeof(float);
        EEPROM.write(addr++, autoRules[i].lastState ? 1 : 0);
    }
    EEPROM.commit();
}

void loadAutomationConfig(void) {
    int addr = EEPROM_AUTO_RULES_ADDR;
    uint8_t activeRules = EEPROM.read(addr++);
    if (activeRules > MAX_AUTO_RULES) {
        initializeAutomationRules();
        return;
    }
    initializeAutomationRules();
    for (int i = 0; i < activeRules && i < MAX_AUTO_RULES; i++) {
        autoRules[i].nodeId = EEPROM.read(addr++);
        autoRules[i].relayId = EEPROM.read(addr++);
        autoRules[i].enabled = EEPROM.read(addr++) == 1;
        autoRules[i].useTempControl = EEPROM.read(addr++) == 1;
        autoRules[i].useHumControl = EEPROM.read(addr++) == 1;
        autoRules[i].logicOR = EEPROM.read(addr++) == 1;
        EEPROM.get(addr, autoRules[i].tempMin);
        addr += sizeof(float);
        EEPROM.get(addr, autoRules[i].tempMax);
        addr += sizeof(float);
        EEPROM.get(addr, autoRules[i].humMin);
        addr += sizeof(float);
        EEPROM.get(addr, autoRules[i].humMax);
        addr += sizeof(float);
        autoRules[i].lastState = EEPROM.read(addr++) == 1;
        if (autoRules[i].nodeId > MAX_NODES || autoRules[i].relayId > 4 || autoRules[i].relayId == 0) {
            autoRules[i].enabled = false;
        }
    }
}

void saveQueueConfig(void) {
    int addr = EEPROM_QUEUE_CONFIG_ADDR;
    EEPROM.write(addr++, queueConfig.stepCount);
    EEPROM.write(addr++, queueConfig.loop ? 1 : 0);
    EEPROM.write(addr++, queueConfig.autoStart ? 1 : 0);
    for (int i = 0; i < MAX_QUEUE_STEPS; i++) {
        if (i < queueConfig.stepCount) {
            EEPROM.write(addr++, queueConfig.steps[i].relayMask);
            EEPROM.put(addr, queueConfig.steps[i].duration);
            addr += sizeof(uint32_t);
        } else {
            EEPROM.write(addr++, 0);
            uint32_t zero = 0;
            EEPROM.put(addr, zero);
            addr += sizeof(uint32_t);
        }
    }
    EEPROM.commit();
}

void loadQueueConfig(void) {
    int addr = EEPROM_QUEUE_CONFIG_ADDR;
    queueConfig.stepCount = EEPROM.read(addr++);
    queueConfig.loop = EEPROM.read(addr++) == 1;
    queueConfig.autoStart = EEPROM.read(addr++) == 1;
    if (queueConfig.stepCount > MAX_QUEUE_STEPS) {
        initializeQueueConfig();
        return;
    }
    for (int i = 0; i < MAX_QUEUE_STEPS; i++) {
        queueConfig.steps[i].relayMask = EEPROM.read(addr++) & 0x0F;
        EEPROM.get(addr, queueConfig.steps[i].duration);
        addr += sizeof(uint32_t);
    }
}

void saveHybridConfig(void) {
    EEPROM.write(EEPROM_HYBRID_CONFIG_ADDR, hybridLogic);
    EEPROM.commit();
}

void loadHybridConfig(void) {
    hybridLogic = EEPROM.read(EEPROM_HYBRID_CONFIG_ADDR);
    if (hybridLogic > 1) {
        hybridLogic = 0;
    }
}

void saveSensorSleep(void) {
    uint16_t seconds = shunya_clamp_sleep_sec(sensorSleepSec);
    EEPROM.write(EEPROM_SENSOR_SLEEP_ADDR, (uint8_t)(seconds & 0xFF));
    EEPROM.write(EEPROM_SENSOR_SLEEP_ADDR + 1, (uint8_t)(seconds >> 8));
    EEPROM.commit();
}

void loadSensorSleep(void) {
    uint16_t seconds = (uint16_t)EEPROM.read(EEPROM_SENSOR_SLEEP_ADDR) |
                       ((uint16_t)EEPROM.read(EEPROM_SENSOR_SLEEP_ADDR + 1) << 8);
    if (seconds == 0xFFFFu || seconds == 0) {
        sensorSleepSec = SENSOR_SLEEP_DEFAULT_S;
        return;
    }
    sensorSleepSec = shunya_clamp_sleep_sec(seconds);
}

void applySensorSleep(uint16_t seconds) {
    sensorSleepSec = shunya_clamp_sleep_sec(seconds);
    saveSensorSleep();
    hubRequestSetSleep(sensorSleepSec);
}

void loadConfigFromEEPROM(void) {
    operationMode = EEPROM.read(EEPROM_MODE_ADDR);
    if (operationMode > MODE_HYBRID) {
        operationMode = MODE_MANUAL;
    }
    loadManualModeConfig();
    loadAutomationConfig();
    loadQueueConfig();
    loadHybridConfig();
    loadSensorSleep();
}

void eepromFlushAll(void) {
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0xFF);
    }
    EEPROM.commit();
}
