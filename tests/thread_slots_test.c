#include "../src/hub/slots.h"
#include "../src/hub/hub_config.h"
#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_blob[sizeof(hub_slot_t) * HUB_MAX_NODES];
static size_t s_blob_size;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)name;
    (void)mode;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value,
                       size_t *length)
{
    (void)handle;
    (void)key;
    if (!s_blob_size || *length != s_blob_size) return ESP_FAIL;
    memcpy(value, s_blob, s_blob_size);
    *length = s_blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    (void)handle;
    (void)key;
    assert(length == sizeof(s_blob));
    memcpy(s_blob, value, length);
    s_blob_size = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

static void make_eui(uint8_t value, uint8_t eui[8])
{
    memset(eui, value, 8);
    eui[0] = 0x02;
}

int main(void)
{
    slots_init();
    assert(slots_count() == 0);

    uint8_t eui[8];
    for (int i = 0; i < HUB_MAX_NODES; ++i) {
        make_eui((uint8_t)(i + 1), eui);
        const int index = slots_alloc(eui, (uint16_t)(0x1000 + i));
        assert(index == i);
        assert(slots_table()[i].node_id == i + 1);
    }
    assert(slots_count() == HUB_MAX_NODES);
    make_eui(99, eui);
    assert(slots_alloc(eui, 0x9999) == -1);

    make_eui(5, eui);
    assert(slots_alloc(eui, 0x7777) == 4);
    assert(slots_table()[4].short_addr == 0x7777);
    assert(slots_count() == HUB_MAX_NODES);

    slots_free_index(7);
    assert(slots_count() == HUB_MAX_NODES - 1);
    make_eui(42, eui);
    assert(slots_alloc(eui, 0x4242) == 7);
    assert(slots_table()[7].node_id == 8);

    slots_update_sensor(7, -4.5f, 88.25f, 3, 87);
    assert(slots_table()[7].last_temp == -4.5f);
    assert(slots_table()[7].last_hum == 88.25f);
    assert(slots_table()[7].lqi == 3);
    assert(slots_table()[7].battery_pct == 87);

    /* Clearing RAM through the public API persists; restore is also tested by
     * first preserving the NVS image and then reloading it. */
    uint8_t preserved[sizeof(s_blob)];
    memcpy(preserved, s_blob, sizeof(preserved));
    slots_clear_all();
    assert(slots_count() == 0);
    memcpy(s_blob, preserved, sizeof(preserved));
    slots_init();
    assert(slots_count() == HUB_MAX_NODES);
    assert(slots_table()[7].short_addr == 0x4242);

    puts("thread slot tests passed");
    return 0;
}
