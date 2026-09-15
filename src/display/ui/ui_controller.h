#ifndef SHUNYA_DISPLAY_UI_CONTROLLER_H
#define SHUNYA_DISPLAY_UI_CONTROLLER_H

void uiControllerBegin();
void uiControllerRefresh();
/* Deterministic immediate refresh for simulator tests and diagnostics. */
void uiControllerRefreshNow();

#endif
