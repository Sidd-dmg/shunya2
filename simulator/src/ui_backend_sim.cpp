#include "hmi/ui_backend_sim.hpp"
#include "hmi/backend.hpp"
#include "ui_backend.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace {

hmi::MockGateway *s_gateway;
display_ack_t s_ack{};
bool s_ack_pending;
display_hmi_ota_status_t s_ota_status{};
bool s_ota_status_pending;
uint16_t s_sequence = 1;
uint32_t s_commands;
std::array<uint32_t, 256> s_command_counts{};

display_snapshot_t convert_snapshot(const hmi::GatewaySnapshot &source) {
    display_snapshot_t out{};
    out.uptime_ms = static_cast<uint32_t>(source.uptime_ms);
    std::copy(source.mac.begin(), source.mac.end(), out.mac);
    std::copy(source.ip.begin(), source.ip.end(), out.ip);
    std::strncpy(out.firmware, source.firmware.c_str(), sizeof(out.firmware) - 1);
    out.wifi_state = source.wifi_connected ? 2 : 0;
    out.wifi_rssi = static_cast<int8_t>(source.wifi_rssi);
    out.mqtt_connected = source.mqtt_connected;
    out.ota_available = source.ota_available;
    out.operation_mode = static_cast<uint8_t>(source.mode);
    out.relay_mask = source.relay_mask;
    out.pairing_open = source.pairing_open;
    out.pairing_remaining_sec = source.pairing_remaining_seconds;
    out.node_count = static_cast<uint8_t>(
        std::min<std::size_t>(source.sensors.size(), DISPLAY_PROTO_MAX_NODES));
    for (uint8_t i = 0; i < out.node_count; ++i) {
        const auto &sensor = source.sensors[i];
        display_node_t &node = out.nodes[i];
        node.node_id = sensor.node_id;
        std::copy(sensor.ieee.begin(), sensor.ieee.end(), node.ieee);
        node.short_addr = sensor.short_address;
        node.temperature_x100 = static_cast<int16_t>(sensor.temperature_c * 100.0f);
        node.humidity_x100 = static_cast<uint16_t>(sensor.humidity_percent * 100.0f);
        node.lqi = sensor.lqi;
        node.age_ms = sensor.age_ms;
        node.active = sensor.active;
        node.battery_percent = sensor.battery_percent;
    }
    out.queue_running = source.queue_running;
    out.queue_paused = source.queue_paused;
    out.queue_loop = source.queue_loop;
    out.queue_autostart = source.queue_autostart;
    out.queue_step_count = static_cast<uint8_t>(
        std::min<std::size_t>(source.queue_steps.size(), 20));
    out.queue_current_step = static_cast<uint8_t>(source.queue_current_step);
    out.queue_remaining_sec = source.queue_remaining_seconds;
    for (uint8_t i = 0; i < out.queue_step_count; ++i) {
        out.queue_steps[i].relay_mask = source.queue_steps[i].relay_mask;
        out.queue_steps[i].duration_sec = source.queue_steps[i].duration_seconds;
    }
    out.rule_count = static_cast<uint8_t>(
        std::min<std::size_t>(source.automation_rules.size(), 16));
    for (uint8_t i = 0; i < out.rule_count; ++i) {
        const auto &input = source.automation_rules[i];
        display_rule_t &rule = out.rules[i];
        rule.enabled = input.enabled;
        rule.node_id = input.node_id;
        rule.relay_id = input.relay_id + 1;
        rule.flags = (input.use_temperature ? 1 : 0) |
                     (input.use_humidity ? 2 : 0) |
                     (input.logic_or ? 4 : 0);
        rule.temp_min_x100 = static_cast<int16_t>(input.temperature_min * 100.0f);
        rule.temp_max_x100 = static_cast<int16_t>(input.temperature_max * 100.0f);
        rule.hum_min_x100 = static_cast<uint16_t>(input.humidity_min * 100.0f);
        rule.hum_max_x100 = static_cast<uint16_t>(input.humidity_max * 100.0f);
        rule.active = input.active;
    }
    out.hybrid_logic_or = source.hybrid_logic_or;
    out.hub_pan_id = source.hub_pan_id;
    out.hub_channel = source.hub_channel;
    out.hub_node_count = out.node_count;
    out.hub_online = source.hub_online;
    out.event_log_pending = source.pending_event_count;
    out.gateway_display_tx = s_commands;
    out.gateway_display_rx = source.revision;
    out.sensor_sleep_sec = source.sensor_sleep_sec;
    return out;
}

hmi::CommandResult submit_command(uint8_t type, const void *payload, uint16_t length) {
    using Type = hmi::Command::Type;
    hmi::Command command{};
    switch (type) {
    case DISPLAY_CMD_RELAY: {
        if (length != sizeof(display_relay_command_t)) return {false, "Bad relay command"};
        display_relay_command_t input;
        std::memcpy(&input, payload, sizeof(input));
        if (input.relay_index == 0xFF) {
            command.type = Type::all_relays_off;
        } else {
            command.type = Type::set_relay;
            command.index = input.relay_index;
            command.enabled = input.state != 0;
        }
        break;
    }
    case DISPLAY_CMD_MODE:
        if (length != 1 || *(const uint8_t *)payload > 3) return {false, "Bad mode"};
        command.type = Type::set_mode;
        command.mode = static_cast<hmi::OperationMode>(*(const uint8_t *)payload);
        break;
    case DISPLAY_CMD_PAIRING:
        if (length != 1) return {false, "Bad pairing command"};
        command.type = Type::set_pairing;
        command.enabled = *(const uint8_t *)payload != 0;
        break;
    case DISPLAY_CMD_FORGET_NODE:
        if (length != 1) return {false, "Bad node command"};
        command.type = Type::forget_node;
        command.index = *(const uint8_t *)payload;
        break;
    case DISPLAY_CMD_FORGET_ALL:
        command.type = Type::forget_all_nodes;
        break;
    case DISPLAY_CMD_QUEUE_CONFIG: {
        if (length < 3) return {false, "Bad queue command"};
        display_queue_config_command_t input{};
        std::memcpy(&input, payload, std::min<std::size_t>(length, sizeof(input)));
        if (input.step_count > 20) return {false, "Too many queue steps"};
        command.type = Type::configure_queue;
        for (uint8_t i = 0; i < input.step_count; ++i) {
            command.queue_steps.push_back(
                {input.steps[i].relay_mask, input.steps[i].duration_sec});
        }
        break;
    }
    case DISPLAY_CMD_QUEUE_CONTROL:
        if (length != 1 || *(const uint8_t *)payload > DISPLAY_QUEUE_RESTART) {
            return {false, "Bad queue action"};
        }
        command.type = Type::control_queue;
        command.queue_action = static_cast<hmi::QueueAction>(*(const uint8_t *)payload);
        break;
    case DISPLAY_CMD_AUTOMATION_CONFIG: {
        if (length < 1) return {false, "Bad automation command"};
        display_automation_config_command_t input{};
        std::memcpy(&input, payload, std::min<std::size_t>(length, sizeof(input)));
        if (input.rule_count > 16) return {false, "Too many rules"};
        command.type = Type::configure_automation;
        for (uint8_t i = 0; i < input.rule_count; ++i) {
            const display_rule_t &rule = input.rules[i];
            command.automation_rules.push_back({
                rule.enabled != 0, rule.node_id,
                static_cast<uint8_t>(rule.relay_id ? rule.relay_id - 1 : 0),
                (rule.flags & 1) != 0, (rule.flags & 2) != 0, (rule.flags & 4) != 0,
                rule.temp_min_x100 / 100.0f, rule.temp_max_x100 / 100.0f,
                rule.hum_min_x100 / 100.0f, rule.hum_max_x100 / 100.0f,
                rule.active != 0
            });
        }
        break;
    }
    case DISPLAY_CMD_HYBRID_CONFIG:
        if (length != 1) return {false, "Bad hybrid command"};
        command.type = Type::set_hybrid_logic;
        command.enabled = *(const uint8_t *)payload != 0;
        break;
    case DISPLAY_CMD_OTA:
        command.type = Type::start_ota;
        break;
    case DISPLAY_CMD_WIFI_RESET:
        command.type = Type::reset_wifi;
        break;
    case DISPLAY_CMD_LINK_TEST:
        command.type = Type::run_link_test;
        break;
    case DISPLAY_CMD_EVENT_LOG:
        return {true, "Event-log command accepted"};
    case DISPLAY_CMD_HMI_OTA:
        if (length != 1 || *(const uint8_t *)payload > 1)
            return {false, "Bad HMI OTA action"};
        s_ota_status = {};
        s_ota_status.stage = 2;
        s_ota_status.result = 1;
        std::strncpy(s_ota_status.message,
                     *(const uint8_t *)payload
                         ? "Simulator: HMI update ready"
                         : "Simulator: HMI firmware is current",
                     sizeof(s_ota_status.message) - 1);
        s_ota_status_pending = true;
        return {true, "HMI OTA simulated"};
    case DISPLAY_CMD_SENSOR_SLEEP: {
        if (length != 2) return {false, "Bad sleep command"};
        const uint16_t seconds = (uint16_t)*(const uint8_t *)payload |
                                 ((uint16_t)*((const uint8_t *)payload + 1) << 8);
        command.type = Type::set_sensor_sleep;
        command.sleep_seconds = seconds;
        break;
    }
    default:
        return {false, "Unsupported command"};
    }
    return s_gateway->submit(command);
}

}  // namespace

namespace hmi {
void attach_ui_backend(MockGateway &gateway) {
    s_gateway = &gateway;
}

std::uint32_t ui_backend_command_count(std::uint8_t type) {
    return s_command_counts[type];
}

void reset_ui_backend_command_counts() {
    s_command_counts.fill(0);
}
}  // namespace hmi

extern "C" bool uiBackendGetSnapshot(display_snapshot_t *snapshot) {
    if (!s_gateway || !snapshot) return false;
    *snapshot = convert_snapshot(s_gateway->snapshot());
    return true;
}

extern "C" bool uiBackendLinkConnected(void) {
    return s_gateway && s_gateway->snapshot().link_connected;
}

extern "C" bool uiBackendTakeAck(display_ack_t *ack) {
    if (!ack || !s_ack_pending) return false;
    *ack = s_ack;
    s_ack_pending = false;
    return true;
}

extern "C" bool uiBackendTakeOtaStatus(display_hmi_ota_status_t *status) {
    if (!status || !s_ota_status_pending) return false;
    *status = s_ota_status;
    s_ota_status_pending = false;
    return true;
}

extern "C" uint16_t uiBackendSendCommand(uint8_t type, const void *payload, uint16_t length) {
    const uint16_t sequence = s_sequence++;
    const hmi::CommandResult result = s_gateway
        ? submit_command(type, payload, length)
        : hmi::CommandResult{false, "Mock gateway unavailable"};
    s_ack = {sequence,
             static_cast<uint8_t>(result.accepted ? DISPLAY_ACK_OK : DISPLAY_ACK_BAD_VALUE),
             type};
    s_ack_pending = true;
    ++s_commands;
    ++s_command_counts[type];
    return sequence;
}

extern "C" uint32_t uiBackendMillis(void) {
    return s_gateway ? static_cast<uint32_t>(s_gateway->snapshot().uptime_ms) : 0;
}
