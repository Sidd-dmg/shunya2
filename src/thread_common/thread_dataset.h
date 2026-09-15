#ifndef SHUNYA_THREAD_DATASET_H
#define SHUNYA_THREAD_DATASET_H

#include <stdint.h>
#include "openthread/instance.h"

#define THREAD_COLLECTOR_ADDRESS "fd71:6e79:6120:1::1"
#define THREAD_SERVICE_PORT       19788u

/* Applies the private, firmware-compiled dataset. Credentials are not exported. */
int thread_dataset_apply(otInstance *instance);
uint16_t thread_dataset_pan_id(void);
uint8_t thread_dataset_channel(void);

#endif
