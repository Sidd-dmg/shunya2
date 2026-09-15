#ifndef HUB_CONFIG_H
#define HUB_CONFIG_H

#define HUB_UART_NUM        UART_NUM_1
#define HUB_UART_TX_GPIO    5
#define HUB_UART_RX_GPIO    4
#define HUB_UART_BAUD       115200
#define HUB_UART_BUF        512
#define HUB_MAX_NODES       16
#define HUB_PAIR_DEFAULT_S  120
#define HUB_NVS_NAMESPACE   "threadhub"
#define HUB_NVS_SLOTS_KEY   "slots"

#define HA_TEMP_CLUSTER     0x0402
#define HA_HUM_CLUSTER      0x0405

#endif
