#include "../src/gateway/persistence.h"
#include "../src/gateway/app_state.h"
#include "../src/gateway/rtos.h"
#include "EEPROM.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

EEPROMClass EEPROM;
TestSerial Serial;
NodeInfo nodes[MAX_NODES];
uint8_t nodeCount;
bool relayStates[RELAY_COUNT];
uint8_t relayPins[RELAY_COUNT];
uint8_t operationMode;
AutomationRule autoRules[MAX_AUTO_RULES];
QueueConfig queueConfig;
uint8_t control_states[7];
uint8_t hybridLogic;
uint16_t sensorSleepSec;
SemaphoreHandle_t eepromMutex;

void hubRequestSetSleep(uint16_t) {}

static void write_legacy_node(int index, uint8_t id)
{
    int address = EEPROM_NODES_START_ADDR + index * EEPROM_NODE_RECORD_SIZE;
    EEPROM.write(address, id);
    for (int byte = 0; byte < 8; ++byte)
        EEPROM.write(address + 1 + byte, (uint8_t)(id * 10 + byte));
    EEPROM.write(address + 9, id);
    EEPROM.write(address + 10, 0xa0);
}

int main()
{
    memset(EEPROM.bytes, 0xff, sizeof(EEPROM.bytes));
    EEPROM.write(EEPROM_NODE_COUNT_ADDR, 4);
    for (int i = 0; i < 4; ++i) write_legacy_node(i, (uint8_t)(i + 1));
    EEPROM.write(EEPROM_MODE_ADDR, MODE_HYBRID);

    loadNodesFromEEPROM();
    assert(nodeCount == 4);
    assert(nodes[3].nodeId == 4);
    assert(nodes[3].shortAddr == 0xa004);
    assert(EEPROM.read(EEPROM_NODE_COUNT_ADDR) == 0);
    assert(EEPROM.read(EEPROM_MODE_ADDR) == MODE_HYBRID);

    memset(nodes, 0, sizeof(nodes));
    nodeCount = 0;
    loadNodesFromEEPROM();
    assert(nodeCount == 4);
    assert(nodes[0].nodeId == 1);

    /* A recognized marker with damaged records must reset v2 instead of
     * resurrecting the now-obsolete legacy table. */
    EEPROM.write(EEPROM_NODES_V2_START_ADDR + 5,
                 (uint8_t)(EEPROM.read(EEPROM_NODES_V2_START_ADDR + 5) ^ 1));
    EEPROM.write(EEPROM_NODE_COUNT_ADDR, 4);
    for (int i = 0; i < 4; ++i) write_legacy_node(i, (uint8_t)(9 + i));
    loadNodesFromEEPROM();
    assert(nodeCount == 0);

    for (int i = 0; i < MAX_NODES; ++i) {
        nodes[i].nodeId = (uint8_t)(i + 1);
        memset(nodes[i].ieee, i + 1, sizeof(nodes[i].ieee));
        nodes[i].shortAddr = (uint16_t)(0x2000 + i);
    }
    nodeCount = MAX_NODES;
    saveNodesToEEPROM();
    memset(nodes, 0, sizeof(nodes));
    nodeCount = 0;
    loadNodesFromEEPROM();
    assert(nodeCount == MAX_NODES);
    assert(nodes[15].nodeId == 16);
    assert(nodes[15].shortAddr == 0x200f);

    puts("persistence migration tests passed");
    return 0;
}
