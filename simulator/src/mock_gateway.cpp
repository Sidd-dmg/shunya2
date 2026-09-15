#include "hmi/backend.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hmi {

MockGateway::MockGateway() {
    state_.firmware = "sim-1.0.0";
    state_.mac = {0x24, 0x6f, 0x28, 0x42, 0x10, 0xa7};
    state_.ip = {192, 168, 1, 84};
    state_.link_connected = true;
    state_.wifi_connected = true;
    state_.wifi_rssi = -51;
    state_.mqtt_connected = true;
    state_.hub_online = true;
    state_.hub_pan_id = 0x4a31;
    state_.hub_channel = 20;
    state_.queue_loop = true;
    state_.queue_steps = {{0x01, 20}, {0x03, 15}, {0x04, 25}, {0x00, 10}};
    state_.queue_remaining_seconds = state_.queue_steps.front().duration_seconds;
    state_.sensors = {
        {1, {0x10, 0x4b, 0x00, 0x28, 0x91, 0x7c, 0x5e, 0x01}, 0x19af, 23.7f, 48.2f, 218, 1200, true, 87},
        {2, {0x10, 0x4b, 0x00, 0x28, 0x91, 0x7c, 0x5e, 0x02}, 0x3c02, 27.1f, 55.6f, 194, 3400, true, 64},
        {3, {0x10, 0x4b, 0x00, 0x28, 0x91, 0x7c, 0x5e, 0x03}, 0x71d4, 19.8f, 61.4f, 176, 8700, true, 0xFF},
    };
    state_.automation_rules = {
        {true, 1, 0, true, false, false, 20.0f, 25.0f, 0.0f, 0.0f, false},
        {true, 2, 1, false, true, false, 0.0f, 0.0f, 45.0f, 60.0f, false},
    };
}

GatewaySnapshot MockGateway::snapshot() const { return state_; }

CommandResult MockGateway::submit(const Command &command) {
    switch (command.type) {
    case Command::Type::set_relay:
        if (state_.mode != OperationMode::manual)
            return {false, "Relay control requires Manual mode"};
        if (command.index >= 4) return {false, "Relay index is out of range"};
        if (command.enabled) state_.relay_mask |= std::uint8_t(1U << command.index);
        else state_.relay_mask &= std::uint8_t(~(1U << command.index));
        break;
    case Command::Type::all_relays_off:
        state_.relay_mask = 0;
        break;
    case Command::Type::set_mode:
        state_.mode = command.mode;
        state_.queue_running = command.mode == OperationMode::queue ||
                               command.mode == OperationMode::hybrid;
        break;
    case Command::Type::set_pairing:
        state_.pairing_open = command.enabled;
        state_.pairing_remaining_seconds = command.enabled ? 180 : 0;
        pairing_phase_ms_ = 0;
        break;
    case Command::Type::forget_node:
        state_.sensors.erase(
            std::remove_if(state_.sensors.begin(), state_.sensors.end(),
                           [&](const Sensor &s) { return s.node_id == command.index; }),
            state_.sensors.end());
        break;
    case Command::Type::forget_all_nodes:
        state_.sensors.clear();
        break;
    case Command::Type::configure_queue:
        if (command.queue_steps.empty() || command.queue_steps.size() > 20)
            return {false, "Queue must contain 1 to 20 steps"};
        state_.queue_steps = command.queue_steps;
        state_.queue_current_step = 0;
        state_.queue_remaining_seconds = state_.queue_steps.front().duration_seconds;
        queue_phase_ms_ = 0;
        break;
    case Command::Type::control_queue:
        if (state_.queue_steps.empty()) return {false, "Queue is not configured"};
        if (command.queue_action == QueueAction::pause) state_.queue_paused = true;
        if (command.queue_action == QueueAction::resume) {
            state_.queue_running = true;
            state_.queue_paused = false;
        }
        if (command.queue_action == QueueAction::stop) {
            state_.queue_running = false;
            state_.queue_paused = false;
            state_.relay_mask = 0;
        }
        if (command.queue_action == QueueAction::restart) {
            state_.queue_running = true;
            state_.queue_paused = false;
            state_.queue_current_step = 0;
            state_.queue_remaining_seconds = state_.queue_steps.front().duration_seconds;
            queue_phase_ms_ = 0;
        }
        break;
    case Command::Type::configure_automation:
        if (command.automation_rules.size() > 16)
            return {false, "At most 16 automation rules are supported"};
        state_.automation_rules = command.automation_rules;
        break;
    case Command::Type::set_hybrid_logic:
        state_.hybrid_logic_or = command.enabled;
        break;
    case Command::Type::start_ota:
        state_.ota_available = false;
        break;
    case Command::Type::reset_wifi:
        state_.wifi_connected = false;
        state_.mqtt_connected = false;
        break;
    case Command::Type::run_link_test:
        state_.pending_event_count++;
        break;
    case Command::Type::set_sensor_sleep:
        if (command.sleep_seconds < 5) state_.sensor_sleep_sec = 5;
        else if (command.sleep_seconds > 3600) state_.sensor_sleep_sec = 3600;
        else state_.sensor_sleep_sec = command.sleep_seconds;
        break;
    }
    publish();
    return {true, "Command accepted by mock gateway"};
}

std::size_t MockGateway::subscribe(SnapshotHandler handler) {
    const auto id = next_subscription_++;
    subscribers_.emplace_back(id, std::move(handler));
    subscribers_.back().second(state_);
    return id;
}

void MockGateway::unsubscribe(std::size_t subscription) {
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                       [&](const auto &entry) { return entry.first == subscription; }),
        subscribers_.end());
}

void MockGateway::setSensorsForTest(std::vector<Sensor> sensors) {
    state_.sensors = std::move(sensors);
    publish();
}

void MockGateway::setConnectivityForTest(bool link, bool wifi, bool mqtt, bool hub) {
    state_.link_connected = link;
    state_.wifi_connected = wifi;
    state_.mqtt_connected = mqtt;
    state_.hub_online = hub;
    publish();
}

void MockGateway::tick(std::uint32_t elapsed_ms) {
    state_.uptime_ms += elapsed_ms;
    sensor_phase_ms_ += elapsed_ms;
    queue_phase_ms_ += elapsed_ms;
    bool changed = false;

    if (sensor_phase_ms_ >= 1000) {
        sensor_phase_ms_ %= 1000;
        const float seconds = static_cast<float>(state_.uptime_ms) / 1000.0f;
        for (std::size_t i = 0; i < state_.sensors.size(); ++i) {
            auto &sensor = state_.sensors[i];
            sensor.temperature_c += 0.04f * std::sin(seconds * 0.31f + static_cast<float>(i));
            sensor.humidity_percent += 0.06f * std::cos(seconds * 0.19f + static_cast<float>(i));
            sensor.age_ms = 150U + static_cast<std::uint32_t>(i) * 420U;
        }
        changed = true;
    }

    if (state_.pairing_open) {
        pairing_phase_ms_ += elapsed_ms;
        while (pairing_phase_ms_ >= 1000 && state_.pairing_remaining_seconds) {
            pairing_phase_ms_ -= 1000;
            --state_.pairing_remaining_seconds;
            changed = true;
        }
        if (!state_.pairing_remaining_seconds) state_.pairing_open = false;
    }

    if (state_.queue_running && !state_.queue_paused && !state_.queue_steps.empty()) {
        auto &step = state_.queue_steps[state_.queue_current_step];
        state_.relay_mask = step.relay_mask;
        const auto duration_ms = static_cast<std::uint64_t>(step.duration_seconds) * 1000U;
        state_.queue_remaining_seconds =
            static_cast<std::uint32_t>((duration_ms - std::min(queue_phase_ms_, duration_ms) + 999U) / 1000U);
        if (queue_phase_ms_ >= duration_ms) {
            queue_phase_ms_ = 0;
            if (++state_.queue_current_step >= state_.queue_steps.size()) {
                state_.queue_current_step = 0;
                state_.queue_running = state_.queue_loop;
            }
        }
        changed = true;
    }
    if (changed) publish();
}

void MockGateway::publish() {
    ++state_.revision;
    for (const auto &entry : subscribers_) entry.second(state_);
}

}  // namespace hmi
