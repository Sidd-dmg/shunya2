#ifndef GATEWAY_DIAGNOSTICS_H
#define GATEWAY_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void diagnosticsBegin(void);
void diagnosticsSetEnabled(bool enabled);
bool diagnosticsEnabled(void);
void diagnosticsLog(const char *process, const char *format, ...);
void diagnosticsHex(const char *process, const char *direction,
                    const uint8_t *data, size_t length);
void diagnosticsTask(void *parameter);

#endif
