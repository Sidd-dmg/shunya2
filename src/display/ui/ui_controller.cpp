#include "ui_controller.h"
#include "ui_backend.h"
#include "generated/ui.h"
#include "shunya_zb.h"
#include <cstdio>
#include <cstring>

static const lv_color_t COLOR_PANEL = lv_color_hex(0x102235);
static const lv_color_t COLOR_PANEL_2 = lv_color_hex(0x1A344D);
static const lv_color_t COLOR_TEXT = lv_color_hex(0xF7FAFC);
static const lv_color_t COLOR_MUTED = lv_color_hex(0x9AAFC2);
static const lv_color_t COLOR_ORANGE = lv_color_hex(0xFF8733);
static const lv_color_t COLOR_GREEN = lv_color_hex(0x34D399);
static const lv_color_t COLOR_BLUE = lv_color_hex(0x38BDF8);
static const lv_color_t COLOR_RED = lv_color_hex(0xEF4444);

static display_snapshot_t s_current;
static display_snapshot_t s_rendered;
static bool s_has_snapshot;
static bool s_has_rendered;
static bool s_link_rendered;
static bool s_has_link_rendered;
static uint32_t s_toast_at;
static uint32_t s_clock_second = UINT32_MAX;
static uint8_t s_last_dimmer_opacity = 0xFF;
static uint8_t s_active_tab = 0xFF;
static uint32_t s_last_normal_refresh;
static uint32_t s_last_slow_refresh;
static uint32_t s_last_housekeeping_refresh;
static uint32_t s_snapshot_received_at;
static display_snapshot_t s_system_rendered;
static bool s_has_system_rendered;
static uint8_t s_sensor_page;
static uint8_t s_sensor_row_node_ids[4];
static char s_sensor_row_text[4][112];
static uint8_t s_sensor_page_rendered = 0xFF;
static uint8_t s_sensor_page_count_rendered = 0xFF;

static constexpr uint8_t HISTORY_CAPACITY = 60;
struct NodeHistory {
    uint8_t node_id;
    uint8_t count;
    uint8_t head;
    uint32_t last_uptime_ms;
    int16_t temperature_x10[HISTORY_CAPACITY];
    int16_t humidity_x10[HISTORY_CAPACITY];
};
static NodeHistory s_histories[DISPLAY_PROTO_MAX_NODES];
static uint8_t s_history_node_id;
static lv_chart_series_t *s_temperature_series;
static lv_chart_series_t *s_humidity_series;

/* Fast feedback is serviced by the 16 ms UI task. Snapshot-driven controls
 * are capped at 20 Hz, dimmer housekeeping at 10 Hz, and static text at 1 Hz. */
static constexpr uint32_t NORMAL_REFRESH_MS = 50;
static constexpr uint32_t HOUSEKEEPING_REFRESH_MS = 100;
static constexpr uint32_t SLOW_REFRESH_MS = 1000;

static void formatX100(char *output, size_t size, int32_t value);

static NodeHistory *historyFor(uint8_t node_id, bool create) {
    NodeHistory *empty = nullptr;
    for (auto &history : s_histories) {
        if (history.node_id == node_id) return &history;
        if (!history.node_id && !empty) empty = &history;
    }
    if (!create || !empty) return nullptr;
    memset(empty, 0, sizeof(*empty));
    empty->node_id = node_id;
    empty->last_uptime_ms = UINT32_MAX;
    return empty;
}

static const display_node_t *nodeById(const display_snapshot_t &snapshot,
                                      uint8_t node_id) {
    for (const auto &node : snapshot.nodes) {
        if (node.node_id == node_id) return &node;
    }
    return nullptr;
}

static void renderHistory(uint8_t node_id) {
    NodeHistory *history = historyFor(node_id, false);
    const display_node_t *node = nodeById(s_current, node_id);
    if (!history || !node) return;

    if (!s_temperature_series) {
        s_temperature_series = lv_chart_add_series(
            ui_history_chart, COLOR_ORANGE, LV_CHART_AXIS_PRIMARY_Y);
        s_humidity_series = lv_chart_add_series(
            ui_history_chart, COLOR_BLUE, LV_CHART_AXIS_SECONDARY_Y);
    }
    const uint8_t point_count = history->count > 1 ? history->count : 2;
    lv_chart_set_point_count(ui_history_chart, point_count);
    lv_chart_set_all_value(ui_history_chart, s_temperature_series,
                           LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ui_history_chart, s_humidity_series,
                           LV_CHART_POINT_NONE);
    const uint8_t start =
        (uint8_t)((history->head + HISTORY_CAPACITY - history->count) %
                  HISTORY_CAPACITY);
    for (uint8_t i = 0; i < history->count; ++i) {
        const uint8_t source = (uint8_t)((start + i) % HISTORY_CAPACITY);
        const uint8_t target = history->count == 1 ? 1 : i;
        lv_chart_set_value_by_id(ui_history_chart, s_temperature_series,
                                 target, history->temperature_x10[source]);
        lv_chart_set_value_by_id(ui_history_chart, s_humidity_series,
                                 target, history->humidity_x10[source]);
    }
    char temperature[12], humidity[12];
    formatX100(temperature, sizeof(temperature), node->temperature_x100);
    formatX100(humidity, sizeof(humidity), node->humidity_x100);
    lv_label_set_text_fmt(ui_history_title, "NODE %u TREND", node_id);
    lv_label_set_text_fmt(ui_history_values, "%s C   |   %s %%RH",
                          temperature, humidity);
    lv_label_set_text_fmt(ui_history_hint,
                          "%u of %u recent samples  |  orange temperature  |  blue humidity",
                          history->count, HISTORY_CAPACITY);
    lv_chart_refresh(ui_history_chart);
}

static void recordHistory(const display_snapshot_t &snapshot) {
    for (const auto &node : snapshot.nodes) {
        if (!node.node_id || !node.active) continue;
        NodeHistory *history = historyFor(node.node_id, true);
        if (!history || history->last_uptime_ms == snapshot.uptime_ms) continue;
        history->temperature_x10[history->head] =
            (int16_t)(node.temperature_x100 / 10);
        history->humidity_x10[history->head] =
            (int16_t)(node.humidity_x100 / 10);
        history->head = (uint8_t)((history->head + 1) % HISTORY_CAPACITY);
        if (history->count < HISTORY_CAPACITY) ++history->count;
        history->last_uptime_ms = snapshot.uptime_ms;
        if (s_history_node_id == node.node_id &&
            !lv_obj_has_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN)) {
            renderHistory(node.node_id);
        }
    }
}

static void formatX100(char *output, size_t size, int32_t value) {
    const bool negative = value < 0;
    const uint32_t magnitude = negative ? (uint32_t)(-value) : (uint32_t)value;
    snprintf(output, size, "%s%lu.%lu", negative ? "-" : "",
             (unsigned long)(magnitude / 100U),
             (unsigned long)((magnitude % 100U) / 10U));
}

static const char *batterySymbol(uint8_t percent) {
    if (percent == BATTERY_PCT_UNKNOWN) return LV_SYMBOL_BATTERY_EMPTY;
    if (percent >= 80) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 55) return LV_SYMBOL_BATTERY_3;
    if (percent >= 30) return LV_SYMBOL_BATTERY_2;
    if (percent >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void toast(const char *text, lv_color_t color) {
    lv_label_set_text(ui_toast, text);
    lv_obj_set_style_bg_color(ui_toast, color, 0);
    lv_obj_set_style_text_color(ui_toast, COLOR_TEXT, 0);
    lv_obj_clear_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_toast);
    s_toast_at = uiBackendMillis();
}

static void animateArc(void *object, int32_t value) {
    lv_arc_set_value((lv_obj_t *)object, (int16_t)value);
}

static void setArcTarget(lv_obj_t *arc, int32_t target, bool animate) {
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    const int32_t current = lv_arc_get_value(arc);
    if (current == target) return;
    lv_anim_del(arc, animateArc);
    if (!animate) {
        lv_arc_set_value(arc, (int16_t)target);
        return;
    }
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, arc);
    lv_anim_set_values(&animation, current, target);
    lv_anim_set_time(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, animateArc);
    lv_anim_start(&animation);
}

static bool nodeChanged(const display_node_t &a, const display_node_t &b) {
    return memcmp(&a, &b, sizeof(display_node_t)) != 0;
}

static uint8_t collectVisibleNodes(const display_snapshot_t &snapshot,
                                   const display_node_t **visible) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < DISPLAY_PROTO_MAX_NODES && count < 2; ++i) {
        if (snapshot.nodes[i].node_id && snapshot.nodes[i].active)
            visible[count++] = &snapshot.nodes[i];
    }
    return count;
}

static void setStatusChip(uint8_t index, bool healthy, bool warning) {
    const lv_color_t color = healthy ? COLOR_GREEN : (warning ? COLOR_ORANGE : COLOR_PANEL_2);
    lv_obj_set_style_bg_color(ui_status_chips[index], color, 0);
    lv_obj_set_style_text_color(ui_status_icons[index],
                                healthy || warning ? COLOR_TEXT : COLOR_MUTED, 0);
    lv_obj_set_style_border_width(ui_status_chips[index], healthy || warning ? 0 : 1, 0);
    lv_obj_set_style_border_color(ui_status_chips[index], COLOR_MUTED, 0);
}

static void refreshLinkStatus(bool link) {
    if (!s_has_link_rendered || link != s_link_rendered) {
        setStatusChip(UI_STATUS_LINK, link, false);
        s_link_rendered = link;
        s_has_link_rendered = true;
    }
}

static void refreshHeader(const display_snapshot_t &snapshot) {
    const bool first = !s_has_rendered;
    if (first || snapshot.wifi_state != s_rendered.wifi_state) {
        setStatusChip(UI_STATUS_WIFI, snapshot.wifi_state == 2, snapshot.wifi_state == 1);
    }
    if (first || snapshot.mqtt_connected != s_rendered.mqtt_connected) {
        setStatusChip(UI_STATUS_MQTT, snapshot.mqtt_connected, false);
    }
    if (first || snapshot.hub_online != s_rendered.hub_online) {
        setStatusChip(UI_STATUS_HUB, snapshot.hub_online, false);
    }
}

static void refreshClock(uint32_t uptime_ms) {
    const uint32_t second = uptime_ms / 1000UL;
    if (second != s_clock_second) {
        lv_label_set_text_fmt(ui_clock, "%02lu:%02lu:%02lu",
                              (unsigned long)(second / 3600UL),
                              (unsigned long)((second / 60UL) % 60UL),
                              (unsigned long)(second % 60UL));
        s_clock_second = second;
    }
}

static void refreshDashboard(const display_snapshot_t &snapshot, bool force) {
    static const char *modes[] = {"MANUAL", "AUTOMATION", "QUEUE", "HYBRID"};
    const bool first = force || !s_has_rendered;
    if (first || snapshot.operation_mode != s_rendered.operation_mode) {
        lv_label_set_text_fmt(ui_mode_status, "%s MODE",
                              modes[snapshot.operation_mode < 4 ? snapshot.operation_mode : 0]);
    }
    if (first || snapshot.queue_running != s_rendered.queue_running ||
        snapshot.queue_paused != s_rendered.queue_paused ||
        snapshot.queue_current_step != s_rendered.queue_current_step ||
        snapshot.queue_step_count != s_rendered.queue_step_count ||
        snapshot.queue_remaining_sec != s_rendered.queue_remaining_sec) {
        if (snapshot.queue_running) {
            lv_label_set_text_fmt(ui_queue_status, "Step %u/%u  |  %lus%s",
                                  snapshot.queue_current_step + 1,
                                  snapshot.queue_step_count,
                                  (unsigned long)snapshot.queue_remaining_sec,
                                  snapshot.queue_paused ? "  PAUSED" : "");
        } else {
            lv_label_set_text(ui_queue_status, "Queue idle");
        }
    }

    const display_node_t *visible[2] = {nullptr, nullptr};
    const display_node_t *old_visible[2] = {nullptr, nullptr};
    const uint8_t count = collectVisibleNodes(snapshot, visible);
    const uint8_t old_count =
        s_has_rendered ? collectVisibleNodes(s_rendered, old_visible) : 0;
    if (first || count != old_count) {
        if (count) lv_obj_add_flag(ui_no_nodes, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(ui_no_nodes, LV_OBJ_FLAG_HIDDEN);
    }

    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (slot >= count) {
            if (first || slot < old_count)
                lv_obj_add_flag(ui_node_panels[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const bool changed = first || slot >= old_count ||
                             nodeChanged(*visible[slot], *old_visible[slot]);
        if (first || count != old_count) {
            lv_obj_clear_flag(ui_node_panels[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_x(ui_node_panels[slot], count == 1 ? 195 : (slot ? 385 : 5));
        }
        if (!changed) continue;
        const display_node_t &node = *visible[slot];
        const display_node_t *old = slot < old_count ? old_visible[slot] : nullptr;
        if (!old || node.node_id != old->node_id || node.active != old->active) {
            lv_label_set_text_fmt(ui_node_titles[slot], "NODE %u%s",
                                  node.node_id, node.active ? "" : "  STALE");
            lv_obj_set_style_border_color(ui_node_panels[slot],
                                          node.active ? COLOR_GREEN : COLOR_ORANGE, 0);
        }
        if (!old || node.temperature_x100 != old->temperature_x100 ||
            node.humidity_x100 != old->humidity_x100) {
            char temperature[12];
            char humidity[12];
            formatX100(temperature, sizeof(temperature), node.temperature_x100);
            formatX100(humidity, sizeof(humidity), node.humidity_x100);
            lv_label_set_text_fmt(ui_temperature_values[slot], "%s\nC", temperature);
            lv_label_set_text_fmt(ui_humidity_values[slot], "%s\n%%RH", humidity);
            const int32_t temperature_percent =
                ((int32_t)node.temperature_x100 + 1000) * 100 / 6000; /* -10..50 C */
            const int32_t humidity_percent = node.humidity_x100 / 100;
            const bool animate = !first && s_active_tab == 0;
            setArcTarget(ui_temperature_arcs[slot], temperature_percent, animate);
            setArcTarget(ui_humidity_arcs[slot], humidity_percent, animate);
        }
        if (!old || node.lqi != old->lqi || node.age_ms / 1000 != old->age_ms / 1000 ||
            node.battery_percent != old->battery_percent) {
            if (node.battery_percent == BATTERY_PCT_UNKNOWN) {
                lv_label_set_text_fmt(ui_node_meta[slot],
                                      "LQI %u  |  %s --  |  updated %lus ago",
                                      node.lqi, batterySymbol(node.battery_percent),
                                      (unsigned long)(node.age_ms / 1000UL));
            } else {
                lv_label_set_text_fmt(ui_node_meta[slot],
                                      "LQI %u  |  %s %u%%  |  updated %lus ago",
                                      node.lqi, batterySymbol(node.battery_percent),
                                      node.battery_percent,
                                      (unsigned long)(node.age_ms / 1000UL));
            }
        }
    }
}

static void refreshRelays(const display_snapshot_t &snapshot, bool force) {
    if (!force && s_has_rendered &&
        snapshot.relay_mask == s_rendered.relay_mask) return;
    for (uint8_t i = 0; i < DISPLAY_RELAY_COUNT; ++i) {
        const bool on = (snapshot.relay_mask & (1u << i)) != 0;
        lv_label_set_text_fmt(ui_relay_labels[i], "RELAY %u\n%s", i + 1, on ? "ON" : "OFF");
        lv_obj_set_style_bg_color(ui_relay_buttons[i], on ? COLOR_GREEN : COLOR_PANEL_2, 0);
    }
}

static uint16_t s_sleep_drag_value = 0;
static bool s_sleep_drag_dirty = false;

static void commitSleepSlider(uint16_t seconds) {
    s_sleep_drag_dirty = false;
    uint8_t payload[2] = {(uint8_t)(seconds & 0xFF), (uint8_t)(seconds >> 8)};
    if (!uiBackendSendCommand(DISPLAY_CMD_SENSOR_SLEEP, payload, 2)) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

static void refreshSensors(const display_snapshot_t &snapshot, bool force) {
    const bool first = force || !s_has_rendered;
    if (first || snapshot.sensor_sleep_sec != s_rendered.sensor_sleep_sec) {
        const uint16_t seconds = snapshot.sensor_sleep_sec
                                     ? snapshot.sensor_sleep_sec
                                     : SENSOR_SLEEP_DEFAULT_S;
        const bool user_adjusting =
            lv_obj_has_state(ui_sleep_slider, LV_STATE_PRESSED) || s_sleep_drag_dirty;
        if (!user_adjusting) {
            lv_label_set_text_fmt(ui_sleep_label, "SLEEP %u s", seconds);
            const int32_t slider_value =
                seconds > 600 ? 600 : (seconds < 5 ? 5 : (int32_t)seconds);
            lv_slider_set_value(ui_sleep_slider, slider_value, LV_ANIM_OFF);
        }
    }
    if (first || snapshot.pairing_open != s_rendered.pairing_open ||
        snapshot.pairing_remaining_sec != s_rendered.pairing_remaining_sec) {
        if (snapshot.pairing_open) {
            lv_label_set_text_fmt(ui_pair_status, "PAIRING OPEN  |  %us",
                                  snapshot.pairing_remaining_sec);
            lv_obj_set_style_text_color(ui_pair_status, COLOR_ORANGE, 0);
        } else {
            lv_label_set_text(ui_pair_status, "PAIRING CLOSED");
            lv_obj_set_style_text_color(ui_pair_status, COLOR_MUTED, 0);
        }
    }
    const display_node_t *paired[DISPLAY_PROTO_MAX_NODES] = {};
    uint8_t paired_count = 0;
    for (uint8_t i = 0; i < DISPLAY_PROTO_MAX_NODES; ++i) {
        if (snapshot.nodes[i].node_id) paired[paired_count++] = &snapshot.nodes[i];
    }
    const uint8_t page_count = paired_count ? (uint8_t)((paired_count + 3) / 4) : 1;
    if (s_sensor_page >= page_count) s_sensor_page = page_count - 1;
    if (force || s_sensor_page != s_sensor_page_rendered ||
        page_count != s_sensor_page_count_rendered) {
        lv_label_set_text_fmt(ui_sensor_page_label, "PAGE %u / %u",
                              s_sensor_page + 1, page_count);
        if (s_sensor_page == 0)
            lv_obj_add_state(ui_sensor_previous_button, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(ui_sensor_previous_button, LV_STATE_DISABLED);
        if (s_sensor_page + 1 >= page_count)
            lv_obj_add_state(ui_sensor_next_button, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(ui_sensor_next_button, LV_STATE_DISABLED);
        s_sensor_page_rendered = s_sensor_page;
        s_sensor_page_count_rendered = page_count;
    }

    const uint8_t page_start = s_sensor_page * 4;
    for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t index = page_start + row;
        const display_node_t *node = index < paired_count ? paired[index] : nullptr;
        s_sensor_row_node_ids[row] = node ? node->node_id : 0;
        char row_text[112];
        if (node) {
            char temperature[12];
            char humidity[12];
            formatX100(temperature, sizeof(temperature), node->temperature_x100);
            formatX100(humidity, sizeof(humidity), node->humidity_x100);
            if (node->battery_percent == BATTERY_PCT_UNKNOWN) {
                snprintf(row_text, sizeof(row_text),
                         "NODE %u   %s C   %s %%RH   %s --   LQI %u   %lus%s",
                         node->node_id, temperature, humidity,
                         batterySymbol(node->battery_percent), node->lqi,
                         (unsigned long)(node->age_ms / 1000UL),
                         node->active ? "" : "   STALE");
            } else {
                snprintf(row_text, sizeof(row_text),
                         "NODE %u   %s C   %s %%RH   %s %u%%   LQI %u   %lus%s",
                         node->node_id, temperature, humidity,
                         batterySymbol(node->battery_percent),
                         node->battery_percent, node->lqi,
                         (unsigned long)(node->age_ms / 1000UL),
                         node->active ? "" : "   STALE");
            }
            lv_obj_clear_flag(ui_forget_node_buttons[row], LV_OBJ_FLAG_HIDDEN);
        } else {
            snprintf(row_text, sizeof(row_text), "Not paired");
            lv_obj_add_flag(ui_forget_node_buttons[row], LV_OBJ_FLAG_HIDDEN);
        }
        if (force || strcmp(row_text, s_sensor_row_text[row]) != 0) {
            lv_label_set_text(ui_node_details[row], row_text);
            snprintf(s_sensor_row_text[row], sizeof(s_sensor_row_text[row]),
                     "%s", row_text);
        }
    }
}

static void relayMaskText(char *output, size_t size, uint8_t mask) {
    if (!mask) {
        snprintf(output, size, "All off");
        return;
    }
    size_t used = 0;
    for (uint8_t i = 0; i < DISPLAY_RELAY_COUNT; ++i) {
        if (mask & (1u << i)) {
            used += (size_t)snprintf(output + used, size - used,
                                     "%sR%u", used ? " + " : "", i + 1);
            if (used >= size) break;
        }
    }
}

static void setTableCell(lv_obj_t *table, uint16_t row, uint16_t column,
                         const char *value) {
    const char *current = lv_table_get_cell_value(table, row, column);
    if (!current || strcmp(current, value) != 0)
        lv_table_set_cell_value(table, row, column, value);
}

static bool queueConfigChanged(const display_snapshot_t &a,
                               const display_snapshot_t &b) {
    return a.queue_step_count != b.queue_step_count ||
           memcmp(a.queue_steps, b.queue_steps, sizeof(a.queue_steps)) != 0;
}

static bool queueRuntimeChanged(const display_snapshot_t &a,
                                const display_snapshot_t &b) {
    return a.queue_current_step != b.queue_current_step ||
           a.queue_running != b.queue_running ||
           a.queue_paused != b.queue_paused ||
           a.queue_remaining_sec != b.queue_remaining_sec;
}

static void refreshQueueTable(const display_snapshot_t &snapshot, bool rebuild) {
    if (rebuild) {
        lv_table_set_row_cnt(ui_queue_table, snapshot.queue_step_count + 1);
        setTableCell(ui_queue_table, 0, 0, "#");
        setTableCell(ui_queue_table, 0, 1, "RELAYS");
        setTableCell(ui_queue_table, 0, 2, "DURATION");
        setTableCell(ui_queue_table, 0, 3, "STATE");
    }
    for (uint8_t i = 0; i < snapshot.queue_step_count; ++i) {
        char number[8];
        char relays[32];
        char duration[24];
        char state[48];
        if (rebuild) {
            snprintf(number, sizeof(number), "%u", i + 1);
            relayMaskText(relays, sizeof(relays), snapshot.queue_steps[i].relay_mask);
            snprintf(duration, sizeof(duration), "%lus",
                     (unsigned long)snapshot.queue_steps[i].duration_sec);
            setTableCell(ui_queue_table, i + 1, 0, number);
            setTableCell(ui_queue_table, i + 1, 1, relays);
            setTableCell(ui_queue_table, i + 1, 2, duration);
        }
        if (snapshot.queue_running && i == snapshot.queue_current_step) {
            snprintf(state, sizeof(state), "%s  |  %lus remaining",
                     snapshot.queue_paused ? "PAUSED" : "RUNNING",
                     (unsigned long)snapshot.queue_remaining_sec);
        } else {
            snprintf(state, sizeof(state), "READY");
        }
        setTableCell(ui_queue_table, i + 1, 3, state);
    }
}

static bool ruleConfigChanged(const display_rule_t &a, const display_rule_t &b) {
    return a.enabled != b.enabled || a.node_id != b.node_id ||
           a.relay_id != b.relay_id || a.flags != b.flags ||
           a.temp_min_x100 != b.temp_min_x100 ||
           a.temp_max_x100 != b.temp_max_x100 ||
           a.hum_min_x100 != b.hum_min_x100 ||
           a.hum_max_x100 != b.hum_max_x100;
}

static bool rulesConfigChanged(const display_snapshot_t &a,
                               const display_snapshot_t &b) {
    if (a.rule_count != b.rule_count) return true;
    for (uint8_t i = 0; i < a.rule_count; ++i) {
        if (ruleConfigChanged(a.rules[i], b.rules[i])) return true;
    }
    return false;
}

static bool rulesRuntimeChanged(const display_snapshot_t &a,
                                const display_snapshot_t &b) {
    if (a.rule_count != b.rule_count) return true;
    for (uint8_t i = 0; i < a.rule_count; ++i) {
        if (a.rules[i].active != b.rules[i].active) return true;
    }
    return false;
}

static void refreshRuleTable(const display_snapshot_t &snapshot, bool rebuild) {
    if (rebuild) {
        lv_table_set_row_cnt(ui_rule_table, snapshot.rule_count + 1);
        setTableCell(ui_rule_table, 0, 0, "#");
        setTableCell(ui_rule_table, 0, 1, "NODE");
        setTableCell(ui_rule_table, 0, 2, "RELAY");
        setTableCell(ui_rule_table, 0, 3, "CONDITION");
        setTableCell(ui_rule_table, 0, 4, "STATE");
    }
    for (uint8_t i = 0; i < snapshot.rule_count; ++i) {
        const display_rule_t &rule = snapshot.rules[i];
        char number[8], node[12], relay[12], condition[96];
        if (rebuild) {
            snprintf(number, sizeof(number), "%u", i + 1);
            snprintf(node, sizeof(node), "N%u", rule.node_id);
            snprintf(relay, sizeof(relay), "R%u", rule.relay_id);
            char tmin[12], tmax[12], hmin[12], hmax[12];
            formatX100(tmin, sizeof(tmin), rule.temp_min_x100);
            formatX100(tmax, sizeof(tmax), rule.temp_max_x100);
            formatX100(hmin, sizeof(hmin), rule.hum_min_x100);
            formatX100(hmax, sizeof(hmax), rule.hum_max_x100);
            if ((rule.flags & 3) == 3) {
                snprintf(condition, sizeof(condition), "T %s-%s C  %s  H %s-%s %%",
                         tmin, tmax, (rule.flags & 4) ? "OR" : "AND", hmin, hmax);
            } else if (rule.flags & 1) {
                snprintf(condition, sizeof(condition), "Temperature %s-%s C", tmin, tmax);
            } else if (rule.flags & 2) {
                snprintf(condition, sizeof(condition), "Humidity %s-%s %%", hmin, hmax);
            } else {
                snprintf(condition, sizeof(condition), "No condition");
            }
            setTableCell(ui_rule_table, i + 1, 0, number);
            setTableCell(ui_rule_table, i + 1, 1, node);
            setTableCell(ui_rule_table, i + 1, 2, relay);
            setTableCell(ui_rule_table, i + 1, 3, condition);
        }
        setTableCell(ui_rule_table, i + 1, 4,
                     !rule.enabled ? "OFF" : (rule.active ? "ACTIVE" : "ARMED"));
    }
}

static void refreshPrograms(const display_snapshot_t &snapshot, bool force) {
    const bool first = force || !s_has_rendered;
    if (first || snapshot.operation_mode != s_rendered.operation_mode) {
        lv_dropdown_set_selected(ui_mode_dropdown, snapshot.operation_mode);
    }
    if (first || snapshot.operation_mode != s_rendered.operation_mode ||
        snapshot.queue_step_count != s_rendered.queue_step_count ||
        snapshot.rule_count != s_rendered.rule_count) {
        static const char *modes[] = {"MANUAL", "AUTOMATION", "QUEUE", "HYBRID"};
        lv_label_set_text_fmt(ui_program_summary, "%s  |  Q %u  |  A %u",
                              modes[snapshot.operation_mode < 4 ? snapshot.operation_mode : 0],
                              snapshot.queue_step_count, snapshot.rule_count);
    }
    const bool queue_config = first || queueConfigChanged(snapshot, s_rendered);
    if (queue_config || queueRuntimeChanged(snapshot, s_rendered))
        refreshQueueTable(snapshot, queue_config);
    const bool rules_config = first || rulesConfigChanged(snapshot, s_rendered);
    if (rules_config || rulesRuntimeChanged(snapshot, s_rendered))
        refreshRuleTable(snapshot, rules_config);
}

static void refreshSystem(const display_snapshot_t &snapshot, bool force) {
    if (!force && s_has_system_rendered &&
        snapshot.uptime_ms / 1000 == s_system_rendered.uptime_ms / 1000 &&
        snapshot.wifi_state == s_system_rendered.wifi_state &&
        snapshot.wifi_rssi == s_system_rendered.wifi_rssi &&
        snapshot.mqtt_connected == s_system_rendered.mqtt_connected &&
        snapshot.hub_pan_id == s_system_rendered.hub_pan_id &&
        snapshot.hub_channel == s_system_rendered.hub_channel &&
        snapshot.hub_node_count == s_system_rendered.hub_node_count &&
        snapshot.event_log_pending == s_system_rendered.event_log_pending &&
        snapshot.gateway_display_tx == s_system_rendered.gateway_display_tx &&
        snapshot.gateway_display_rx == s_system_rendered.gateway_display_rx &&
        snapshot.gateway_display_crc_errors ==
            s_system_rendered.gateway_display_crc_errors) return;
    lv_label_set_text_fmt(
        ui_system_info,
        "FW %s  |  Uptime %lus  |  IP %u.%u.%u.%u  |  RSSI %d dBm\n"
        "Hub PAN 0x%04X / CH %u / %u nodes  |  MQTT %s  |  Logs %lu\n"
        "Display UART TX %lu / RX %lu / CRC %lu",
        snapshot.firmware, (unsigned long)(snapshot.uptime_ms / 1000UL),
        snapshot.ip[0], snapshot.ip[1], snapshot.ip[2], snapshot.ip[3],
        snapshot.wifi_rssi, snapshot.hub_pan_id, snapshot.hub_channel,
        snapshot.hub_node_count, snapshot.mqtt_connected ? "ONLINE" : "OFFLINE",
        (unsigned long)snapshot.event_log_pending,
        (unsigned long)snapshot.gateway_display_tx,
        (unsigned long)snapshot.gateway_display_rx,
        (unsigned long)snapshot.gateway_display_crc_errors);
    s_system_rendered = snapshot;
    s_has_system_rendered = true;
}

static void refreshSnapshot(const display_snapshot_t &snapshot,
                            bool active_tab_changed) {
    refreshHeader(snapshot);
    const uint8_t active = (uint8_t)lv_tabview_get_tab_act(ui_main_tabs);
    const bool force = active_tab_changed || !s_has_rendered;
    switch (active) {
    case 0: refreshDashboard(snapshot, force); break;
    case 1: refreshRelays(snapshot, force); break;
    case 2: refreshSensors(snapshot, force); break;
    case 3: refreshPrograms(snapshot, force); break;
    case 4: refreshSystem(snapshot, force); break;
    default: break;
    }
    s_active_tab = active;
    s_rendered = snapshot;
    s_has_rendered = true;
}

static void sendSimple(uint8_t type, uint8_t value) {
    if (uiBackendSendCommand(type, &value, 1)) {
        toast("Command sent", COLOR_ORANGE);
    } else {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

static void confirmationResult(lv_event_t *event) {
    lv_obj_t *box = lv_event_get_current_target(event);
    const char *button = lv_msgbox_get_active_btn_text(box);
    if (button && strcmp(button, "Confirm") == 0) {
        const uint16_t packed = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
        const uint8_t type = (uint8_t)(packed >> 8);
        const uint8_t value = (uint8_t)(packed & 0xFF);
        if (uiBackendSendCommand(type, &value, 1)) {
            toast("Destructive command sent", COLOR_ORANGE);
        } else {
            toast("Command queue busy - please retry", COLOR_RED);
        }
    }
    lv_msgbox_close(box);
}

static void showConfirmation(uint8_t type, uint8_t value) {
    static const char *buttons[] = {"Cancel", "Confirm", ""};
    const char *message = type == DISPLAY_CMD_HMI_OTA
        ? "The HMI will connect to Wi-Fi, install its display firmware, and restart."
        : "This operation may erase configuration or restart the gateway.";
    lv_obj_t *box = lv_msgbox_create(nullptr, "Confirm action",
                                     message,
                                     buttons, true);
    lv_obj_set_width(box, 480);
    lv_obj_center(box);
    lv_obj_add_event_cb(box, confirmationResult, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)(((uint16_t)type << 8) | value));
}

void uiControllerBegin() {
    shunya_ui_init();
}

static void refreshController(bool force) {
    const uint32_t now = uiBackendMillis();
    refreshLinkStatus(uiBackendLinkConnected());

    /* Critical tier (60 Hz): command feedback and short-lived overlays. */
    display_ack_t ack;
    if (uiBackendTakeAck(&ack)) {
        if (ack.command != DISPLAY_CMD_WIFI_PROV || ack.status != DISPLAY_ACK_OK) {
            toast(ack.status == DISPLAY_ACK_OK ? "Command accepted" : "Command rejected",
                  ack.status == DISPLAY_ACK_OK ? COLOR_GREEN : COLOR_RED);
        }
    }
    display_wifi_prov_t wifi_prov;
    if (uiBackendTakeWifiProv(&wifi_prov)) {
        wifi_prov.name[sizeof(wifi_prov.name) - 1] = '\0';
        wifi_prov.payload[sizeof(wifi_prov.payload) - 1] = '\0';
        const uint32_t payload_len = (uint32_t)strlen(wifi_prov.payload);
        if (!payload_len ||
            lv_qrcode_update(ui_wifi_qr, wifi_prov.payload, payload_len) != LV_RES_OK) {
            toast("Provisioning QR could not be drawn", COLOR_RED);
        } else {
            lv_label_set_text(ui_wifi_qr_name,
                              wifi_prov.name[0] ? wifi_prov.name : "Shunya");
            lv_obj_clear_flag(ui_wifi_qr_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ui_wifi_qr_overlay);
        }
    }
    display_hmi_ota_status_t ota_status;
    if (uiBackendTakeOtaStatus(&ota_status)) {
        ota_status.message[sizeof(ota_status.message) - 1] = '\0';
        toast(ota_status.message,
              ota_status.result == 1 ? COLOR_GREEN :
              (ota_status.result == 2 ? COLOR_RED : COLOR_ORANGE));
    }
    if (s_toast_at && now - s_toast_at > 2200UL) {
        lv_obj_add_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_at = 0;
    }

    /* Normal tier (20 Hz): consume each immutable snapshot once and update
     * only the visible tab. A newly selected tab is force-populated once. */
    if (force || !s_has_snapshot ||
        now - s_last_normal_refresh >= NORMAL_REFRESH_MS) {
        s_last_normal_refresh = now;
        display_snapshot_t snapshot;
        const bool received = uiBackendGetSnapshot(&snapshot);
        if (received) {
            s_current = snapshot;
            s_has_snapshot = true;
            s_snapshot_received_at = now;
            recordHistory(snapshot);
        }
        const uint8_t active = (uint8_t)lv_tabview_get_tab_act(ui_main_tabs);
        const bool tab_changed = active != s_active_tab;
        if (s_has_snapshot && (force || received || tab_changed)) {
            refreshSnapshot(s_current, tab_changed);
        }
    }

    /* Slow tier (1 Hz): locally interpolate the clock between gateway
     * snapshots and refresh the text-heavy System page only while visible. */
    if (s_has_snapshot &&
        (force || now - s_last_slow_refresh >= SLOW_REFRESH_MS)) {
        s_last_slow_refresh = now;
        display_snapshot_t interpolated = s_current;
        interpolated.uptime_ms += now - s_snapshot_received_at;
        refreshClock(interpolated.uptime_ms);
        if (s_active_tab == 4) refreshSystem(interpolated, false);
    }

    /* Static housekeeping (10 Hz): changing opacity invalidates a large
     * region, so never evaluate or redraw the dimmer at frame rate. */
    if (force ||
        now - s_last_housekeeping_refresh >= HOUSEKEEPING_REFRESH_MS) {
        s_last_housekeeping_refresh = now;
        static const uint32_t timeouts[] = {0, 60000UL, 300000UL, 900000UL};
        const uint8_t timeout_index =
            (uint8_t)lv_dropdown_get_selected(ui_timeout_dropdown);
        const uint32_t inactive = lv_disp_get_inactive_time(nullptr);
        uint8_t opacity =
            (uint8_t)((100 - lv_slider_get_value(ui_brightness_slider)) * 2);
        if (timeout_index && inactive >= timeouts[timeout_index]) opacity = 235;
        if (opacity != s_last_dimmer_opacity) {
            lv_obj_set_style_bg_opa(ui_dimmer, opacity, 0);
            s_last_dimmer_opacity = opacity;
        }
        if (s_sleep_drag_dirty &&
            !lv_obj_has_state(ui_sleep_slider, LV_STATE_PRESSED)) {
            commitSleepSlider(s_sleep_drag_value);
        }
    }
}

void uiControllerRefresh() {
    refreshController(false);
}

void uiControllerRefreshNow() {
    refreshController(true);
}

extern "C" void action_status_detail(lv_event_t *event) {
    const uint8_t status = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    char detail[96];
    switch (status) {
    case UI_STATUS_LINK:
        snprintf(detail, sizeof(detail), "Gateway display link: %s",
                 uiBackendLinkConnected() ? "connected" : "offline");
        break;
    case UI_STATUS_WIFI:
        snprintf(detail, sizeof(detail), "Wi-Fi: %s  |  RSSI %d dBm",
                 s_current.wifi_state == 2 ? "connected" :
                 (s_current.wifi_state == 1 ? "connecting" : "offline"),
                 s_current.wifi_rssi);
        break;
    case UI_STATUS_MQTT:
        snprintf(detail, sizeof(detail), "MQTT broker: %s",
                 s_current.mqtt_connected ? "connected" : "offline");
        break;
    default:
        snprintf(detail, sizeof(detail), "Zigbee hub: %s  |  PAN 0x%04X  CH %u",
                 s_current.hub_online ? "online" : "offline",
                 s_current.hub_pan_id, s_current.hub_channel);
        break;
    }
    toast(detail, COLOR_ORANGE);
}

extern "C" void action_node_history(lv_event_t *event) {
    if (!s_has_snapshot) return;
    const uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    const display_node_t *visible[2] = {nullptr, nullptr};
    const uint8_t count = collectVisibleNodes(s_current, visible);
    if (slot >= count || !visible[slot]) return;
    s_history_node_id = visible[slot]->node_id;
    renderHistory(s_history_node_id);
    lv_obj_clear_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_history_overlay);
}

extern "C" void action_close_history(lv_event_t *) {
    lv_obj_add_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN);
    s_history_node_id = 0;
}

extern "C" void action_relay_toggle(lv_event_t *event) {
    const uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (!s_has_snapshot || s_current.operation_mode != 0) {
        toast("Relay control requires Manual mode", COLOR_ORANGE);
        return;
    }
    const display_relay_command_t command = {
        index, (uint8_t)((s_current.relay_mask & (1u << index)) ? 0 : 1)
    };
    if (!uiBackendSendCommand(DISPLAY_CMD_RELAY, &command, sizeof(command))) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_all_relays_off(lv_event_t *) {
    const display_relay_command_t command = {0xFF, 0};
    if (!uiBackendSendCommand(DISPLAY_CMD_RELAY, &command, sizeof(command))) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_pairing_toggle(lv_event_t *) {
    const uint8_t open = s_current.pairing_open ? 0 : 1;
    if (!uiBackendSendCommand(DISPLAY_CMD_PAIRING, &open, 1)) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_forget_node(lv_event_t *event) {
    const uint8_t row = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    const uint8_t node = row < 4 ? s_sensor_row_node_ids[row] : 0;
    if (!node) return;
    if (!uiBackendSendCommand(DISPLAY_CMD_FORGET_NODE, &node, 1)) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_sensor_previous(lv_event_t *) {
    if (s_sensor_page) {
        --s_sensor_page;
        if (s_has_snapshot) refreshSensors(s_current, true);
    }
}

extern "C" void action_sensor_next(lv_event_t *) {
    const uint8_t page_count =
        s_current.node_count ? (uint8_t)((s_current.node_count + 3) / 4) : 1;
    if (s_sensor_page + 1 < page_count) {
        ++s_sensor_page;
        if (s_has_snapshot) refreshSensors(s_current, true);
    }
}

extern "C" void action_sleep_changed(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    const uint16_t seconds = (uint16_t)lv_slider_get_value(ui_sleep_slider);
    lv_label_set_text_fmt(ui_sleep_label, "SLEEP %u s", seconds);
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (lv_indev_get_act()) {
            s_sleep_drag_value = seconds;
            s_sleep_drag_dirty = true;
        }
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    commitSleepSlider(seconds);
}

extern "C" void action_forget_all_nodes(lv_event_t *) {
    showConfirmation(DISPLAY_CMD_FORGET_ALL, 0);
}

extern "C" void action_apply_mode(lv_event_t *) {
    const uint8_t mode = (uint8_t)lv_dropdown_get_selected(ui_mode_dropdown);
    if (!uiBackendSendCommand(DISPLAY_CMD_MODE, &mode, 1)) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_ota_check(lv_event_t *) {
    sendSimple(DISPLAY_CMD_HMI_OTA, 0);
}
extern "C" void action_ota_update(lv_event_t *) {
    showConfirmation(DISPLAY_CMD_HMI_OTA, 1);
}
extern "C" void action_send_logs(lv_event_t *) { sendSimple(DISPLAY_CMD_EVENT_LOG, 0); }
extern "C" void action_clear_logs(lv_event_t *) { showConfirmation(DISPLAY_CMD_EVENT_LOG, 1); }
extern "C" void action_test_hub(lv_event_t *) { sendSimple(DISPLAY_CMD_LINK_TEST, 1); }
extern "C" void action_test_display(lv_event_t *) { sendSimple(DISPLAY_CMD_LINK_TEST, 2); }
extern "C" void action_reset_wifi(lv_event_t *) { showConfirmation(DISPLAY_CMD_WIFI_RESET, 1); }

extern "C" void action_wifi_qr(lv_event_t *) {
    if (!uiBackendLinkConnected()) {
        toast("Gateway link required", COLOR_ORANGE);
        return;
    }
    if (!uiBackendSendCommand(DISPLAY_CMD_WIFI_PROV, nullptr, 0)) {
        toast("Command queue busy - please retry", COLOR_RED);
    }
}

extern "C" void action_close_wifi_qr(lv_event_t *) {
    lv_obj_add_flag(ui_wifi_qr_overlay, LV_OBJ_FLAG_HIDDEN);
}
extern "C" void action_brightness_changed(lv_event_t *) {}
extern "C" void action_timeout_changed(lv_event_t *) {}
