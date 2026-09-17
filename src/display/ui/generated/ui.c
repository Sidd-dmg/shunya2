#include "ui.h"
#include <stdint.h>

#define COLOR_BG       lv_color_hex(0x07131F)
#define COLOR_PANEL    lv_color_hex(0x102235)
#define COLOR_PANEL_2  lv_color_hex(0x1A344D)
#define COLOR_TEXT     lv_color_hex(0xF7FAFC)
#define COLOR_MUTED    lv_color_hex(0x9AAFC2)
#define COLOR_ORANGE   lv_color_hex(0xFF8733)
#define COLOR_GREEN    lv_color_hex(0x34D399)
#define COLOR_BLUE     lv_color_hex(0x38BDF8)

lv_obj_t *ui_screen;
lv_obj_t *ui_header;
lv_obj_t *ui_header_logo;
lv_obj_t *ui_status_chips[UI_STATUS_COUNT];
lv_obj_t *ui_status_icons[UI_STATUS_COUNT];
lv_obj_t *ui_clock;
lv_obj_t *ui_main_tabs;
lv_obj_t *ui_toast;
lv_obj_t *ui_dimmer;
lv_obj_t *ui_tab_dashboard;
lv_obj_t *ui_tab_relays;
lv_obj_t *ui_tab_sensors;
lv_obj_t *ui_tab_programs;
lv_obj_t *ui_tab_system;
lv_obj_t *ui_mode_status;
lv_obj_t *ui_queue_status;
lv_obj_t *ui_dashboard_nodes;
lv_obj_t *ui_node_panels[2];
lv_obj_t *ui_node_titles[2];
lv_obj_t *ui_temperature_arcs[2];
lv_obj_t *ui_temperature_values[2];
lv_obj_t *ui_humidity_arcs[2];
lv_obj_t *ui_humidity_values[2];
lv_obj_t *ui_node_meta[2];
lv_obj_t *ui_no_nodes;
lv_obj_t *ui_history_overlay;
lv_obj_t *ui_history_card;
lv_obj_t *ui_history_title;
lv_obj_t *ui_history_values;
lv_obj_t *ui_history_hint;
lv_obj_t *ui_history_chart;
lv_obj_t *ui_history_close_button;
lv_obj_t *ui_relay_buttons[4];
lv_obj_t *ui_relay_labels[4];
lv_obj_t *ui_all_off_button;
lv_obj_t *ui_pair_status;
lv_obj_t *ui_pair_toggle_button;
lv_obj_t *ui_forget_all_button;
lv_obj_t *ui_node_rows[4];
lv_obj_t *ui_node_details[4];
lv_obj_t *ui_forget_node_buttons[4];
lv_obj_t *ui_sensor_previous_button;
lv_obj_t *ui_sensor_next_button;
lv_obj_t *ui_sensor_page_label;
lv_obj_t *ui_sleep_label;
lv_obj_t *ui_sleep_slider;
lv_obj_t *ui_mode_dropdown;
lv_obj_t *ui_apply_mode_button;
lv_obj_t *ui_program_summary;
lv_obj_t *ui_program_tabs;
lv_obj_t *ui_queue_tab;
lv_obj_t *ui_automation_tab;
lv_obj_t *ui_queue_table;
lv_obj_t *ui_rule_table;
lv_obj_t *ui_system_info;
lv_obj_t *ui_ota_check_button;
lv_obj_t *ui_ota_update_button;
lv_obj_t *ui_send_logs_button;
lv_obj_t *ui_clear_logs_button;
lv_obj_t *ui_test_hub_button;
lv_obj_t *ui_test_display_button;
lv_obj_t *ui_reset_wifi_button;
lv_obj_t *ui_brightness_slider;
lv_obj_t *ui_timeout_dropdown;

static void style_panel(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, COLOR_PANEL_2, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_set_style_text_color(obj, COLOR_TEXT, 0);
}

static void style_tab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tab, COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(tab, 10, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);
    lv_obj_set_style_pad_column(tab, 10, 0);
}

static void style_tab_buttons(lv_obj_t *tabview) {
    lv_obj_t *buttons = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(buttons, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttons, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(buttons, COLOR_MUTED, LV_PART_ITEMS);
    lv_obj_set_style_text_color(buttons, COLOR_ORANGE,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(buttons, COLOR_PANEL,
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(buttons, 3,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(buttons, LV_BORDER_SIDE_TOP,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(buttons, COLOR_ORANGE,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t callback, void *data) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 128, 46);
    lv_obj_set_style_bg_color(button, COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_color(button, COLOR_ORANGE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, COLOR_ORANGE, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2C4A64), 0);
    lv_obj_set_style_border_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    lv_obj_center(make_label(button, text, &lv_font_montserrat_16));
    return button;
}

static lv_obj_t *make_status_chip(lv_obj_t *parent, const char *symbol, uint8_t index) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 44, 34);
    lv_obj_set_style_bg_color(button, COLOR_PANEL_2, 0);
    lv_obj_set_style_bg_color(button, COLOR_ORANGE, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 17, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, action_status_detail, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_t *icon = make_label(button, symbol, &lv_font_montserrat_20);
    lv_obj_center(icon);
    ui_status_icons[index] = icon;
    return button;
}

static lv_obj_t *make_arc(lv_obj_t *parent, lv_color_t color) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 126, 126);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_rotation(arc, 135);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COLOR_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static void create_dashboard(void) {
    style_tab(ui_tab_dashboard);
    lv_obj_clear_flag(ui_tab_dashboard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *summary = lv_obj_create(ui_tab_dashboard);
    lv_obj_set_pos(summary, 10, 6);
    lv_obj_set_size(summary, 760, 54);
    style_panel(summary);
    lv_obj_set_style_pad_hor(summary, 18, 0);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summary, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_mode_status = make_label(summary, "MANUAL MODE", &lv_font_montserrat_20);
    ui_queue_status = make_label(summary, "Queue idle", &lv_font_montserrat_16);
    lv_obj_set_style_text_color(ui_queue_status, COLOR_MUTED, 0);

    ui_dashboard_nodes = lv_obj_create(ui_tab_dashboard);
    lv_obj_set_pos(ui_dashboard_nodes, 10, 70);
    lv_obj_set_size(ui_dashboard_nodes, 760, 244);
    lv_obj_set_style_bg_opa(ui_dashboard_nodes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_dashboard_nodes, 0, 0);
    lv_obj_set_style_pad_all(ui_dashboard_nodes, 0, 0);
    lv_obj_clear_flag(ui_dashboard_nodes, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < 2; ++i) {
        lv_obj_t *panel = lv_obj_create(ui_dashboard_nodes);
        lv_obj_set_pos(panel, i ? 385 : 5, 0);
        lv_obj_set_size(panel, 370, 238);
        style_panel(panel);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(panel, COLOR_PANEL_2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(panel, action_node_history, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        ui_node_panels[i] = panel;

        ui_node_titles[i] = make_label(panel, "NODE", &lv_font_montserrat_20);
        lv_obj_align(ui_node_titles[i], LV_ALIGN_TOP_MID, 0, -2);

        ui_temperature_arcs[i] = make_arc(panel, COLOR_ORANGE);
        lv_obj_set_pos(ui_temperature_arcs[i], 18, 38);
        ui_temperature_values[i] = make_label(panel, "--.-\nC", &lv_font_montserrat_20);
        lv_obj_align_to(ui_temperature_values[i], ui_temperature_arcs[i],
                        LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(ui_temperature_values[i], LV_TEXT_ALIGN_CENTER, 0);

        ui_humidity_arcs[i] = make_arc(panel, COLOR_GREEN);
        lv_obj_set_pos(ui_humidity_arcs[i], 202, 38);
        ui_humidity_values[i] = make_label(panel, "--.-\n%RH", &lv_font_montserrat_20);
        lv_obj_align_to(ui_humidity_values[i], ui_humidity_arcs[i],
                        LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(ui_humidity_values[i], LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *temp_caption = make_label(panel, "TEMPERATURE", &lv_font_montserrat_14);
        lv_obj_align_to(temp_caption, ui_temperature_arcs[i], LV_ALIGN_OUT_BOTTOM_MID, 0, -3);
        lv_obj_set_style_text_color(temp_caption, COLOR_ORANGE, 0);
        lv_obj_t *hum_caption = make_label(panel, "HUMIDITY", &lv_font_montserrat_14);
        lv_obj_align_to(hum_caption, ui_humidity_arcs[i], LV_ALIGN_OUT_BOTTOM_MID, 0, -3);
        lv_obj_set_style_text_color(hum_caption, COLOR_GREEN, 0);

        ui_node_meta[i] = make_label(panel, "LQI --  |  waiting", &lv_font_montserrat_14);
        lv_obj_align(ui_node_meta[i], LV_ALIGN_BOTTOM_MID, 0, 1);
        lv_obj_set_style_text_color(ui_node_meta[i], COLOR_MUTED, 0);

        lv_obj_t *tap_hint = make_label(panel, "TAP FOR TREND", &lv_font_montserrat_14);
        lv_obj_align(tap_hint, LV_ALIGN_TOP_RIGHT, 0, 1);
        lv_obj_set_style_text_color(tap_hint, COLOR_BLUE, 0);
    }

    ui_no_nodes = make_label(ui_dashboard_nodes,
                             "No paired sensors\nOpen Sensors to start pairing",
                             &lv_font_montserrat_20);
    lv_obj_set_style_text_align(ui_no_nodes, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_no_nodes, LV_ALIGN_CENTER, 0, 0);
}

static void create_relays(void) {
    style_tab(ui_tab_relays);
    lv_obj_set_flex_flow(ui_tab_relays, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(ui_tab_relays, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t i = 0; i < 4; ++i) {
        ui_relay_buttons[i] = lv_btn_create(ui_tab_relays);
        lv_obj_set_size(ui_relay_buttons[i], 365, 108);
        lv_obj_set_style_radius(ui_relay_buttons[i], 14, 0);
        lv_obj_set_style_bg_color(ui_relay_buttons[i], COLOR_PANEL_2, 0);
        lv_obj_set_style_bg_color(ui_relay_buttons[i], COLOR_ORANGE, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(ui_relay_buttons[i], 0, 0);
        lv_obj_add_event_cb(ui_relay_buttons[i], action_relay_toggle,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        ui_relay_labels[i] = make_label(ui_relay_buttons[i], "RELAY\nOFF",
                                        &lv_font_montserrat_24);
        lv_obj_set_style_text_align(ui_relay_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(ui_relay_labels[i]);
    }
    ui_all_off_button = make_button(ui_tab_relays, "ALL OFF",
                                    action_all_relays_off, NULL);
}

static void create_sensors(void) {
    style_tab(ui_tab_sensors);
    lv_obj_set_flex_flow(ui_tab_sensors, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui_tab_sensors, 6, 0);
    lv_obj_t *toolbar = lv_obj_create(ui_tab_sensors);
    lv_obj_set_size(toolbar, lv_pct(100), 54);
    style_panel(toolbar);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_pair_status = make_label(toolbar, "Pairing closed", &lv_font_montserrat_18);
    ui_pair_toggle_button = make_button(toolbar, "OPEN / CLOSE",
                                        action_pairing_toggle, NULL);
    ui_forget_all_button = make_button(toolbar, "FORGET ALL",
                                       action_forget_all_nodes, NULL);
    lv_obj_t *sleep_bar = lv_obj_create(ui_tab_sensors);
    lv_obj_set_size(sleep_bar, lv_pct(100), 48);
    style_panel(sleep_bar);
    lv_obj_set_flex_flow(sleep_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sleep_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_sleep_label = make_label(sleep_bar, "SLEEP 20 s", &lv_font_montserrat_16);
    lv_obj_set_width(ui_sleep_label, 140);
    ui_sleep_slider = lv_slider_create(sleep_bar);
    lv_obj_set_flex_grow(ui_sleep_slider, 1);
    lv_obj_set_height(ui_sleep_slider, 12);
    lv_slider_set_range(ui_sleep_slider, 5, 600);
    lv_slider_set_value(ui_sleep_slider, 20, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui_sleep_slider, COLOR_ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_sleep_slider, COLOR_ORANGE, LV_PART_KNOB);
    lv_obj_add_event_cb(ui_sleep_slider, action_sleep_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_sleep_slider, action_sleep_changed, LV_EVENT_RELEASED, NULL);
    for (uint8_t i = 0; i < 4; ++i) {
        ui_node_rows[i] = lv_obj_create(ui_tab_sensors);
        lv_obj_set_size(ui_node_rows[i], lv_pct(100), 48);
        style_panel(ui_node_rows[i]);
        lv_obj_set_flex_flow(ui_node_rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ui_node_rows[i], LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui_node_details[i] = make_label(ui_node_rows[i], "Node slot empty",
                                        &lv_font_montserrat_16);
        lv_obj_set_flex_grow(ui_node_details[i], 1);
        ui_forget_node_buttons[i] = make_button(ui_node_rows[i], "FORGET",
                                                action_forget_node,
                                                (void *)(uintptr_t)i);
        lv_obj_set_size(ui_forget_node_buttons[i], 104, 34);
    }
    lv_obj_t *pager = lv_obj_create(ui_tab_sensors);
    lv_obj_set_size(pager, lv_pct(100), 42);
    style_panel(pager);
    lv_obj_set_style_pad_ver(pager, 3, 0);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_sensor_previous_button = make_button(pager, "PREVIOUS",
                                            action_sensor_previous, NULL);
    lv_obj_set_size(ui_sensor_previous_button, 124, 32);
    ui_sensor_page_label = make_label(pager, "PAGE 1 / 1",
                                      &lv_font_montserrat_16);
    ui_sensor_next_button = make_button(pager, "NEXT",
                                        action_sensor_next, NULL);
    lv_obj_set_size(ui_sensor_next_button, 124, 32);
}

static void style_table(lv_obj_t *table) {
    lv_obj_set_style_bg_color(table, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, COLOR_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(table, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, COLOR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, COLOR_PANEL_2, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 7, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 8, LV_PART_ITEMS);
}

static void table_draw(lv_event_t *event) {
    lv_obj_draw_part_dsc_t *draw = lv_event_get_draw_part_dsc(event);
    if (!draw || draw->part != LV_PART_ITEMS || !draw->rect_dsc || !draw->label_dsc)
        return;
    const uint16_t columns = lv_table_get_col_cnt(lv_event_get_target(event));
    if (draw->id < columns) {
        draw->rect_dsc->bg_color = COLOR_PANEL_2;
        draw->rect_dsc->bg_opa = LV_OPA_COVER;
        draw->label_dsc->color = COLOR_ORANGE;
    }
}

static void create_programs(void) {
    style_tab(ui_tab_programs);
    lv_obj_clear_flag(ui_tab_programs, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mode_row = lv_obj_create(ui_tab_programs);
    lv_obj_set_pos(mode_row, 10, 5);
    lv_obj_set_size(mode_row, 760, 58);
    style_panel(mode_row);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mode_label = make_label(mode_row, "OPERATING MODE", &lv_font_montserrat_16);
    lv_obj_set_pos(mode_label, 4, 8);
    ui_mode_dropdown = lv_dropdown_create(mode_row);
    lv_dropdown_set_options(ui_mode_dropdown, "Manual\nAutomation\nQueue\nHybrid");
    lv_obj_set_pos(ui_mode_dropdown, 166, -4);
    lv_obj_set_size(ui_mode_dropdown, 205, 42);
    ui_apply_mode_button = make_button(mode_row, "APPLY MODE",
                                       action_apply_mode, NULL);
    lv_obj_set_pos(ui_apply_mode_button, 386, -4);
    lv_obj_set_size(ui_apply_mode_button, 150, 42);
    ui_program_summary = make_label(mode_row, "Read-only configuration",
                                    &lv_font_montserrat_14);
    lv_obj_set_pos(ui_program_summary, 552, 9);
    lv_obj_set_width(ui_program_summary, 180);
    lv_obj_set_style_text_align(ui_program_summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(ui_program_summary, COLOR_MUTED, 0);

    ui_program_tabs = lv_tabview_create(ui_tab_programs, LV_DIR_TOP, 38);
    lv_obj_set_pos(ui_program_tabs, 10, 72);
    lv_obj_set_size(ui_program_tabs, 760, 294);
    lv_obj_set_style_anim_time(ui_program_tabs, 70, 0);
    style_tab_buttons(ui_program_tabs);
    ui_queue_tab = lv_tabview_add_tab(ui_program_tabs, "QUEUE STEPS");
    ui_automation_tab = lv_tabview_add_tab(ui_program_tabs, "AUTOMATION RULES");
    lv_obj_set_style_pad_all(ui_queue_tab, 4, 0);
    lv_obj_set_style_pad_all(ui_automation_tab, 4, 0);
    lv_obj_set_style_bg_color(ui_queue_tab, COLOR_BG, 0);
    lv_obj_set_style_bg_color(ui_automation_tab, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(ui_queue_tab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(ui_automation_tab, LV_OPA_COVER, 0);

    ui_queue_table = lv_table_create(ui_queue_tab);
    lv_obj_set_size(ui_queue_table, lv_pct(100), lv_pct(100));
    lv_table_set_col_cnt(ui_queue_table, 4);
    lv_table_set_col_width(ui_queue_table, 0, 62);
    lv_table_set_col_width(ui_queue_table, 1, 185);
    lv_table_set_col_width(ui_queue_table, 2, 130);
    lv_table_set_col_width(ui_queue_table, 3, 330);
    style_table(ui_queue_table);
    lv_obj_add_event_cb(ui_queue_table, table_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);

    ui_rule_table = lv_table_create(ui_automation_tab);
    lv_obj_set_size(ui_rule_table, lv_pct(100), lv_pct(100));
    lv_table_set_col_cnt(ui_rule_table, 5);
    lv_table_set_col_width(ui_rule_table, 0, 60);
    lv_table_set_col_width(ui_rule_table, 1, 70);
    lv_table_set_col_width(ui_rule_table, 2, 75);
    lv_table_set_col_width(ui_rule_table, 3, 390);
    lv_table_set_col_width(ui_rule_table, 4, 100);
    style_table(ui_rule_table);
    lv_obj_add_event_cb(ui_rule_table, table_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
}

static void create_system(void) {
    style_tab(ui_tab_system);
    lv_obj_set_flex_flow(ui_tab_system, LV_FLEX_FLOW_COLUMN);
    ui_system_info = make_label(ui_tab_system, "Waiting for gateway...",
                                &lv_font_montserrat_16);
    lv_obj_t *buttons = lv_obj_create(ui_tab_system);
    lv_obj_set_size(buttons, lv_pct(100), 220);
    style_panel(buttons);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_ota_check_button = make_button(buttons, "HMI CHECK", action_ota_check, NULL);
    ui_ota_update_button = make_button(buttons, "HMI UPDATE", action_ota_update, NULL);
    ui_send_logs_button = make_button(buttons, "SEND LOGS", action_send_logs, NULL);
    ui_clear_logs_button = make_button(buttons, "CLEAR LOGS", action_clear_logs, NULL);
    ui_test_hub_button = make_button(buttons, "TEST HUB", action_test_hub, NULL);
    ui_test_display_button = make_button(buttons, "TEST DISPLAY",
                                         action_test_display, NULL);
    ui_reset_wifi_button = make_button(buttons, "RESET WI-FI",
                                        action_reset_wifi, NULL);
    make_label(buttons, "BRIGHTNESS", &lv_font_montserrat_14);
    /* The knob overhangs the ends of the track, so the slider sits in a
     * transparent holder that reserves that overhang. Without it the knob
     * collides with the BRIGHTNESS and TIMEOUT labels either side in the row.
     * LVGL 8 has no margin styles, so spacing has to come from a parent. */
    lv_obj_t *brightness_holder = lv_obj_create(buttons);
    lv_obj_set_size(brightness_holder, 204, 34);
    lv_obj_set_style_bg_opa(brightness_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brightness_holder, 0, 0);
    lv_obj_set_style_pad_all(brightness_holder, 0, 0);
    lv_obj_clear_flag(brightness_holder, LV_OBJ_FLAG_SCROLLABLE);

    ui_brightness_slider = lv_slider_create(brightness_holder);
    /* A slim 12 px track reads better next to the buttons. The knob keeps a
     * finger-sized footprint through its own padding, and the extended click
     * area restores the touch height the thicker track used to provide. */
    lv_obj_set_size(ui_brightness_slider, 176, 12);
    lv_obj_center(ui_brightness_slider);
    lv_slider_set_range(ui_brightness_slider, 10, 100);
    lv_slider_set_value(ui_brightness_slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_radius(ui_brightness_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_brightness_slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightness_slider, COLOR_ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightness_slider, COLOR_ORANGE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui_brightness_slider, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(ui_brightness_slider, 14);
    lv_obj_add_event_cb(ui_brightness_slider, action_brightness_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    make_label(buttons, "TIMEOUT", &lv_font_montserrat_14);
    ui_timeout_dropdown = lv_dropdown_create(buttons);
    lv_dropdown_set_options(ui_timeout_dropdown,
                            "Never\n1 minute\n5 minutes\n15 minutes");
    lv_obj_add_event_cb(ui_timeout_dropdown, action_timeout_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *shunya_ui_create(lv_obj_t *parent) {
    ui_screen = parent ? parent : lv_obj_create(NULL);
    lv_obj_set_size(ui_screen, 800, 480);
    lv_obj_set_style_bg_color(ui_screen, COLOR_BG, 0);
    lv_obj_set_style_border_width(ui_screen, 0, 0);
    lv_obj_set_style_pad_all(ui_screen, 0, 0);
    lv_obj_clear_flag(ui_screen, LV_OBJ_FLAG_SCROLLABLE);

    ui_header = lv_obj_create(ui_screen);
    lv_obj_set_pos(ui_header, 0, 0);
    lv_obj_set_size(ui_header, 800, 52);
    lv_obj_set_style_bg_color(ui_header, COLOR_PANEL, 0);
    lv_obj_set_style_border_width(ui_header, 0, 0);
    lv_obj_set_style_pad_hor(ui_header, 12, 0);
    lv_obj_set_style_pad_ver(ui_header, 8, 0);
    lv_obj_set_flex_flow(ui_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* The logo is taller than the padded content area, so keep the bar from
     * treating it as scrollable content and drawing a scrollbar. */
    lv_obj_clear_flag(ui_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ui_header, LV_SCROLLBAR_MODE_OFF);
    ui_status_chips[UI_STATUS_LINK] = make_status_chip(
        ui_header, LV_SYMBOL_REFRESH, UI_STATUS_LINK);
    ui_status_chips[UI_STATUS_WIFI] = make_status_chip(
        ui_header, LV_SYMBOL_WIFI, UI_STATUS_WIFI);
    ui_status_chips[UI_STATUS_MQTT] = make_status_chip(
        ui_header, LV_SYMBOL_UPLOAD, UI_STATUS_MQTT);
    ui_status_chips[UI_STATUS_HUB] = make_status_chip(
        ui_header, LV_SYMBOL_SHUFFLE, UI_STATUS_HUB);

    /* Clock and logo sit outside the flex row so chips stay left, uptime
     * stays right, and the brand mark remains centred on the bar. */
    ui_clock = make_label(ui_header, "00:00:00", &lv_font_montserrat_16);
    lv_obj_add_flag(ui_clock, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(ui_clock, LV_ALIGN_RIGHT_MID, 0, 0);

    ui_header_logo = lv_img_create(ui_header);
    lv_img_set_src(ui_header_logo, &ui_img_shunya_logo);
    lv_obj_add_flag(ui_header_logo, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ui_header_logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ui_header_logo, LV_ALIGN_CENTER, 0, 0);

    ui_main_tabs = lv_tabview_create(ui_screen, LV_DIR_BOTTOM, 54);
    lv_obj_set_pos(ui_main_tabs, 0, 52);
    lv_obj_set_size(ui_main_tabs, 800, 428);
    lv_obj_set_style_bg_color(ui_main_tabs, COLOR_BG, 0);
    lv_obj_set_style_anim_time(ui_main_tabs, 80, 0);
    style_tab_buttons(ui_main_tabs);
    ui_tab_dashboard = lv_tabview_add_tab(ui_main_tabs, "Dashboard");
    ui_tab_relays = lv_tabview_add_tab(ui_main_tabs, "Relays");
    ui_tab_sensors = lv_tabview_add_tab(ui_main_tabs, "Sensors");
    ui_tab_programs = lv_tabview_add_tab(ui_main_tabs, "Programs");
    ui_tab_system = lv_tabview_add_tab(ui_main_tabs, "System");
    create_dashboard();
    create_relays();
    create_sensors();
    create_programs();
    create_system();

    ui_toast = make_label(ui_screen, "", &lv_font_montserrat_16);
    lv_obj_set_size(ui_toast, 480, 44);
    lv_obj_align(ui_toast, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(ui_toast, COLOR_ORANGE, 0);
    lv_obj_set_style_bg_opa(ui_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui_toast, 12, 0);
    lv_obj_set_style_pad_all(ui_toast, 11, 0);
    lv_obj_set_style_text_align(ui_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);

    ui_dimmer = lv_obj_create(ui_screen);
    lv_obj_set_size(ui_dimmer, 800, 480);
    lv_obj_set_style_bg_color(ui_dimmer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_dimmer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_dimmer, 0, 0);
    lv_obj_clear_flag(ui_dimmer, LV_OBJ_FLAG_CLICKABLE);

    ui_history_overlay = lv_obj_create(ui_screen);
    lv_obj_set_size(ui_history_overlay, 800, 480);
    lv_obj_set_style_bg_color(ui_history_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_history_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ui_history_overlay, 0, 0);
    lv_obj_set_style_pad_all(ui_history_overlay, 0, 0);
    lv_obj_clear_flag(ui_history_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_history_overlay, action_close_history,
                        LV_EVENT_CLICKED, NULL);

    ui_history_card = lv_obj_create(ui_history_overlay);
    lv_obj_set_size(ui_history_card, 650, 338);
    lv_obj_center(ui_history_card);
    style_panel(ui_history_card);
    lv_obj_set_style_border_width(ui_history_card, 2, 0);
    lv_obj_set_style_border_color(ui_history_card, lv_color_hex(0x2A4A64), 0);
    lv_obj_clear_flag(ui_history_card, LV_OBJ_FLAG_SCROLLABLE);

    ui_history_title = make_label(ui_history_card, "NODE TREND",
                                  &lv_font_montserrat_24);
    lv_obj_set_pos(ui_history_title, 10, 4);
    ui_history_values = make_label(ui_history_card, "--.- C   |   --.- %RH",
                                   &lv_font_montserrat_18);
    lv_obj_set_pos(ui_history_values, 10, 38);
    ui_history_hint = make_label(ui_history_card, "Recent display-session history",
                                 &lv_font_montserrat_14);
    lv_obj_set_pos(ui_history_hint, 10, 67);
    lv_obj_set_style_text_color(ui_history_hint, COLOR_MUTED, 0);

    ui_history_close_button = make_button(ui_history_card, LV_SYMBOL_CLOSE,
                                          action_close_history, NULL);
    lv_obj_set_size(ui_history_close_button, 42, 38);
    lv_obj_align(ui_history_close_button, LV_ALIGN_TOP_RIGHT, 0, 0);

    ui_history_chart = lv_chart_create(ui_history_card);
    lv_obj_set_pos(ui_history_chart, 10, 98);
    lv_obj_set_size(ui_history_chart, 606, 205);
    lv_chart_set_type(ui_history_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui_history_chart, 60);
    lv_chart_set_div_line_count(ui_history_chart, 5, 6);
    lv_chart_set_range(ui_history_chart, LV_CHART_AXIS_PRIMARY_Y, -100, 500);
    lv_chart_set_range(ui_history_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 1000);
    lv_obj_set_style_bg_color(ui_history_chart, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_history_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_history_chart, COLOR_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_history_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_history_chart, 10, LV_PART_MAIN);
    lv_obj_set_style_line_color(ui_history_chart, COLOR_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_line_width(ui_history_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(ui_history_chart, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN);

    if (!parent) lv_scr_load(ui_screen);
    return ui_screen;
}

void shunya_ui_init(void) {
    (void)shunya_ui_create(NULL);
}
