/*
 * The native simulator deliberately uses the display firmware's exact LVGL
 * configuration: RGB565, fonts, DPI and widgets. The simulator heap is larger
 * than the old 128 KiB pool so the provisioning QR canvas fits.
 */
#include "../src/display/lv_conf.h"
