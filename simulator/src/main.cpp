#include "hmi/backend.hpp"
#include "hmi/ui_backend_sim.hpp"
#include "display_protocol.h"
#include "generated/ui.h"
#include "ui_controller.h"

#include <lvgl.h>
#include <sdl/sdl.h>
#include <sdl/sdl_common.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr int kDrawLines = 48;

bool save_screenshot(const char *path) {
    /* SDL's partial-flush renderer does not guarantee that unchanged regions
     * remain in the readback target. Force one complete redraw so captures
     * are deterministic and represent the whole physical panel. */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    SDL_Window *window = SDL_GetWindowFromID(1);
    SDL_Renderer *renderer = window ? SDL_GetRenderer(window) : nullptr;
    if (!renderer) return false;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, kWidth, kHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return false;
    const bool ok = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                         surface->pixels, surface->pitch) == 0 &&
                    SDL_SaveBMP(surface, path) == 0;
    SDL_FreeSurface(surface);
    return ok;
}

bool color_is(lv_obj_t *object, lv_color_t expected) {
    return lv_color_to32(lv_obj_get_style_bg_color(object, LV_PART_MAIN)) ==
           lv_color_to32(expected);
}

void capture_tab(const std::filesystem::path &directory, std::uint16_t tab,
                 const char *name) {
    lv_tabview_set_act(ui_main_tabs, tab, LV_ANIM_OFF);
    uiControllerRefreshNow();
    lv_timer_handler();
    const auto path = directory / (std::string(name) + ".bmp");
    if (!save_screenshot(path.string().c_str()))
        throw std::runtime_error(std::string(name) + " capture failed");
}

std::vector<hmi::Sensor> make_sensors(std::size_t count) {
    std::vector<hmi::Sensor> sensors;
    sensors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        hmi::Sensor sensor{};
        sensor.node_id = static_cast<std::uint8_t>(i + 1);
        sensor.ieee = {0x10, 0x4b, 0x00, 0x28, 0x91, 0x7c, 0x5e,
                       static_cast<std::uint8_t>(i + 1)};
        sensor.short_address = static_cast<std::uint16_t>(0x1900 + i);
        sensor.temperature_c = 20.0f + static_cast<float>(i);
        sensor.humidity_percent = 40.0f + static_cast<float>(i);
        sensor.lqi = static_cast<std::uint8_t>(220 - i);
        sensor.age_ms = static_cast<std::uint32_t>(500 + i * 100);
        sensor.active = true;
        sensors.push_back(sensor);
    }
    return sensors;
}

void register_sdl_drivers() {
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    /* Prevent Windows display scaling: one SDL pixel must equal one host pixel. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
#ifdef SDL_HINT_WINDOWS_DPI_SCALING
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif
    sdl_init();

    static std::vector<lv_color_t> pixels(kWidth * kDrawLines);
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, pixels.data(), nullptr, pixels.size());

    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.draw_buf = &draw_buffer;
    display_driver.flush_cb = sdl_display_flush;
    display_driver.hor_res = kWidth;
    display_driver.ver_res = kHeight;
    lv_disp_drv_register(&display_driver);

    static lv_indev_drv_t pointer_driver;
    lv_indev_drv_init(&pointer_driver);
    pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&pointer_driver);

    static lv_indev_drv_t keyboard_driver;
    lv_indev_drv_init(&keyboard_driver);
    keyboard_driver.type = LV_INDEV_TYPE_KEYPAD;
    keyboard_driver.read_cb = sdl_keyboard_read;
    lv_indev_drv_register(&keyboard_driver);
}

}  // namespace

int main(int argc, char **argv) {
    try {
#ifdef _WIN32
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
        lv_init();
        register_sdl_drivers();

        hmi::MockGateway gateway;
        hmi::attach_ui_backend(gateway);
        uiControllerBegin();
        uiControllerRefreshNow();

        if (argc > 1 && std::string(argv[1]) == "--self-test") {
            const auto original = gateway.snapshot();
            if (lv_obj_has_flag(ui_node_panels[0], LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_has_flag(ui_node_panels[1], LV_OBJ_FLAG_HIDDEN) ||
                std::string(lv_label_get_text(ui_node_titles[0])).find("NODE 1") == std::string::npos ||
                std::string(lv_label_get_text(ui_node_titles[1])).find("NODE 2") == std::string::npos) {
                throw std::runtime_error("two-node dashboard limit self-test failed");
            }

            gateway.setSensorsForTest(make_sensors(1));
            uiControllerRefreshNow();
            if (lv_obj_has_flag(ui_node_panels[0], LV_OBJ_FLAG_HIDDEN) ||
                !lv_obj_has_flag(ui_node_panels[1], LV_OBJ_FLAG_HIDDEN)) {
                throw std::runtime_error("one-node dashboard self-test failed");
            }
            gateway.setSensorsForTest({});
            uiControllerRefreshNow();
            if (!lv_obj_has_flag(ui_node_panels[0], LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_has_flag(ui_no_nodes, LV_OBJ_FLAG_HIDDEN)) {
                throw std::runtime_error("zero-node dashboard self-test failed");
            }

            for (const std::size_t count : {std::size_t{4}, std::size_t{5},
                                            std::size_t{16}}) {
                gateway.setSensorsForTest(make_sensors(count));
                lv_tabview_set_act(ui_main_tabs, 2, LV_ANIM_OFF);
                uiControllerRefreshNow();
                const std::size_t pages = (count + 3) / 4;
                const std::string expected =
                    "PAGE 1 / " + std::to_string(pages);
                if (lv_label_get_text(ui_sensor_page_label) != expected) {
                    throw std::runtime_error("sensor fixture pagination self-test failed");
                }
            }

            gateway.setSensorsForTest(make_sensors(16));
            for (int page = 1; page < 4; ++page)
                lv_event_send(ui_sensor_next_button, LV_EVENT_CLICKED, nullptr);
            if (std::string(lv_label_get_text(ui_sensor_page_label)) != "PAGE 4 / 4" ||
                std::string(lv_label_get_text(ui_node_details[0])).find("NODE 13") ==
                    std::string::npos) {
                throw std::runtime_error("sixteen-node next-page self-test failed");
            }

            gateway.setSensorsForTest(make_sensors(5));
            uiControllerRefreshNow();
            if (std::string(lv_label_get_text(ui_sensor_page_label)) != "PAGE 2 / 2" ||
                std::string(lv_label_get_text(ui_node_details[0])).find("NODE 5") ==
                    std::string::npos) {
                throw std::runtime_error("five-node page clamp setup failed");
            }
            lv_event_send(ui_forget_node_buttons[0], LV_EVENT_CLICKED, nullptr);
            uiControllerRefreshNow();
            if (gateway.snapshot().sensors.size() != 4 ||
                std::string(lv_label_get_text(ui_sensor_page_label)) != "PAGE 1 / 1") {
                throw std::runtime_error("displayed-node forget/page clamp self-test failed");
            }

            auto radial_sensors = make_sensors(2);
            gateway.setSensorsForTest(radial_sensors);
            lv_tabview_set_act(ui_main_tabs, 0, LV_ANIM_OFF);
            uiControllerRefreshNow();
            const int16_t old_arc = lv_arc_get_value(ui_temperature_arcs[0]);
            radial_sensors[0].temperature_c = 45.0f;
            gateway.setSensorsForTest(radial_sensors);
            uiControllerRefreshNow();
            lv_tick_inc(200);
            lv_timer_handler();
            if (lv_arc_get_value(ui_temperature_arcs[0]) <= old_arc) {
                throw std::runtime_error("radial animation self-test failed");
            }
            lv_event_send(ui_node_panels[0], LV_EVENT_CLICKED, nullptr);
            if (lv_obj_has_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN) ||
                std::string(lv_label_get_text(ui_history_title)).find("NODE 1") ==
                    std::string::npos) {
                throw std::runtime_error("node history open self-test failed");
            }
            lv_event_send(ui_history_close_button, LV_EVENT_CLICKED, nullptr);
            if (!lv_obj_has_flag(ui_history_overlay, LV_OBJ_FLAG_HIDDEN)) {
                throw std::runtime_error("node history close self-test failed");
            }

            const lv_color_t healthy = lv_color_hex(0x34D399);
            const lv_color_t offline = lv_color_hex(0x1A344D);
            if (!color_is(ui_status_chips[UI_STATUS_LINK], healthy) ||
                !color_is(ui_status_chips[UI_STATUS_WIFI], healthy) ||
                !color_is(ui_status_chips[UI_STATUS_MQTT], healthy) ||
                !color_is(ui_status_chips[UI_STATUS_HUB], healthy)) {
                throw std::runtime_error("healthy status-icon self-test failed");
            }
            gateway.setConnectivityForTest(false, false, false, false);
            uiControllerRefreshNow();
            for (auto *chip : ui_status_chips) {
                if (!color_is(chip, offline))
                    throw std::runtime_error("offline status-icon self-test failed");
            }
            lv_event_send(ui_status_chips[UI_STATUS_WIFI], LV_EVENT_CLICKED, nullptr);
            if (lv_obj_has_flag(ui_toast, LV_OBJ_FLAG_HIDDEN) ||
                std::string(lv_label_get_text(ui_toast)).find("Wi-Fi") == std::string::npos) {
                throw std::runtime_error("status detail self-test failed");
            }
            gateway.setConnectivityForTest(true, true, true, true);
            uiControllerRefreshNow();

            for (std::uint16_t tab = 0; tab < 5; ++tab) {
                lv_tabview_set_act(ui_main_tabs, tab, LV_ANIM_OFF);
                uiControllerRefreshNow();
                if (lv_tabview_get_tab_act(ui_main_tabs) != tab) {
                    throw std::runtime_error("tab navigation self-test failed");
                }
            }
            lv_tabview_set_act(ui_main_tabs, 1, LV_ANIM_OFF);
            lv_event_send(ui_relay_buttons[0], LV_EVENT_CLICKED, nullptr);
            if ((gateway.snapshot().relay_mask & 0x01U) == 0) {
                throw std::runtime_error("relay action self-test failed");
            }
            lv_event_send(ui_pair_toggle_button, LV_EVENT_CLICKED, nullptr);
            if (!gateway.snapshot().pairing_open) {
                throw std::runtime_error("pairing action self-test failed");
            }

            lv_dropdown_set_selected(ui_mode_dropdown, 1);
            lv_event_send(ui_apply_mode_button, LV_EVENT_CLICKED, nullptr);
            if (gateway.snapshot().mode != hmi::OperationMode::automation) {
                throw std::runtime_error("mode selection self-test failed");
            }
            if (lv_table_get_row_cnt(ui_queue_table) != original.queue_steps.size() + 1 ||
                lv_table_get_row_cnt(ui_rule_table) != original.automation_rules.size() + 1 ||
                std::string(lv_table_get_cell_value(ui_queue_table, 0, 1)) != "RELAYS" ||
                std::string(lv_table_get_cell_value(ui_rule_table, 0, 3)) != "CONDITION") {
                throw std::runtime_error("read-only program table self-test failed");
            }
            hmi::reset_ui_backend_command_counts();
            lv_event_send(ui_queue_table, LV_EVENT_CLICKED, nullptr);
            lv_event_send(ui_rule_table, LV_EVENT_CLICKED, nullptr);
            if (hmi::ui_backend_command_count(DISPLAY_CMD_QUEUE_CONFIG) ||
                hmi::ui_backend_command_count(DISPLAY_CMD_QUEUE_CONTROL) ||
                hmi::ui_backend_command_count(DISPLAY_CMD_AUTOMATION_CONFIG) ||
                hmi::ui_backend_command_count(DISPLAY_CMD_HYBRID_CONFIG)) {
                throw std::runtime_error("program tables emitted a write command");
            }
            lv_event_send(ui_ota_check_button, LV_EVENT_CLICKED, nullptr);
            uiControllerRefreshNow();
            if (std::string(lv_label_get_text(ui_toast)).find("Simulator: HMI") ==
                std::string::npos) {
                throw std::runtime_error("HMI OTA feedback self-test failed");
            }

            const auto tab_start = std::chrono::steady_clock::now();
            lv_tabview_set_act(ui_main_tabs, 0, LV_ANIM_ON);
            lv_tick_inc(140);
            lv_timer_handler();
            const auto tab_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - tab_start).count();
            std::printf("HMI simulator self-test passed: conditional nodes, status icons, "
                        "0/1/4/5/16 sensor pagination and forget, radials, mode-only "
                        "Programs, read-only tables; tab render %lld us\n",
                        static_cast<long long>(tab_us));
            return 0;
        }

        if (argc > 2 && std::string(argv[1]) == "--capture-all") {
            const std::filesystem::path directory(argv[2]);
            std::filesystem::create_directories(directory);

            for (int i = 0; i < 12; ++i) {
                gateway.tick(1000);
                uiControllerRefreshNow();
            }
            capture_tab(directory, 0, "dashboard");
            lv_event_send(ui_node_panels[0], LV_EVENT_CLICKED, nullptr);
            lv_timer_handler();
            if (!save_screenshot((directory / "node-trend.bmp").string().c_str()))
                throw std::runtime_error("node trend capture failed");
            lv_event_send(ui_history_close_button, LV_EVENT_CLICKED, nullptr);

            capture_tab(directory, 1, "relays");
            gateway.setSensorsForTest(make_sensors(5));
            capture_tab(directory, 2, "sensors");
            capture_tab(directory, 3, "programs-queue");
            lv_tabview_set_act(ui_program_tabs, 1, LV_ANIM_OFF);
            uiControllerRefreshNow();
            lv_timer_handler();
            if (!save_screenshot((directory / "programs-automation.bmp").string().c_str()))
                throw std::runtime_error("Programs Automation capture failed");
            capture_tab(directory, 4, "system");
            std::puts("HMI screenshots captured for every screen");
            return 0;
        }

        auto previous = std::chrono::steady_clock::now();

        while (!sdl_quit_qry) {
            const auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous);
            previous = now;
            const auto elapsed_ms = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(elapsed.count(), 1, 100));

            lv_tick_inc(elapsed_ms);
            gateway.tick(elapsed_ms);
            uiControllerRefresh();
            const auto wait_ms = std::clamp<std::uint32_t>(lv_timer_handler(), 1, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "HMI simulator failed: %s\n", error.what());
        return 1;
    }
}
