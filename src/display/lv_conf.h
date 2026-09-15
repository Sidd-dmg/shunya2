#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
/* LVGL's pool must not sit in internal SRAM. As a 128 KiB static array it left
 * only 18 KiB free, and an mbedTLS handshake allocates two 16 KiB I/O buffers
 * (MBEDTLS_SSL_MAX_CONTENT_LEN), so HMI OTA failed the TLS connect with
 * MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00). The board carries 8 MiB of OPI PSRAM
 * at 80 MHz and already renders from PSRAM draw buffers, so widget and style
 * allocations live there too and internal SRAM stays free for Wi-Fi and TLS.
 * The native simulator has no PSRAM and keeps the built-in allocator. */
#ifdef ESP_PLATFORM
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC(size) \
    heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(pointer, size) \
    heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (128U * 1024U)
#endif
#define LV_TICK_CUSTOM 0
#define LV_DPI_DEF 130

/* Presentation is paced at 60 Hz. LVGL still renders only invalidated areas;
 * this period controls animation and display refresh timers, not continuous
 * full-screen drawing. Touch is sampled at the same cadence for low latency. */
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 16

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_THEME_DEFAULT 1
#define LV_USE_ANIMATION 1
#define LV_USE_SHADOW 0
#define LV_USE_IMG_TRANSFORM 0
#define LV_USE_USER_DATA 1

#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 1
#define LV_USE_TEXTAREA 0

#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 1
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_ANIMIMG 0
#define LV_USE_GRID 0

#endif
