#ifndef SHUNYA_WAVESHARE_PANEL_H
#define SHUNYA_WAVESHARE_PANEL_H

#include <stdbool.h>

bool wavesharePanelBegin();
bool waveshareLvglLock(int timeout_ms);
void waveshareLvglUnlock();

#endif
