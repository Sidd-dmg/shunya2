#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hmi {

enum class OperationMode : std::uint8_t { manual, automation, queue, hybrid };
enum class QueueAction : std::uint8_t { pause, resume, stop, restart };

struct Sensor {
    std::uint8_t node_id{};
    std::array<std::uint8_t, 8> ieee{};
    std::uint16_t short_address{};
    float temperature_c{};
    float humidity_percent{};
    std::uint8_t lqi{};
    std::uint32_t age_ms{};
    bool active{};
    std::uint8_t battery_percent{0xFF};
};

struct QueueStep {
    std::uint8_t relay_mask{};
    std::uint32_t duration_seconds{};
};

struct AutomationRule {
    bool enabled{};
    std::uint8_t node_id{};
    std::uint8_t relay_id{};
    bool use_temperature{};
    bool use_humidity{};
    bool logic_or{};
    float temperature_min{};
    float temperature_max{};
    float humidity_min{};
    float humidity_max{};
    bool active{};
};

struct GatewaySnapshot {
    std::uint64_t revision{};
    std::uint64_t uptime_ms{};
    std::array<std::uint8_t, 6> mac{};
    std::array<std::uint8_t, 4> ip{};
    std::string firmware;
    bool link_connected{};
    bool wifi_connected{};
    int wifi_rssi{};
    bool mqtt_connected{};
    bool ota_available{};
    OperationMode mode{OperationMode::manual};
    std::uint8_t relay_mask{};
    bool pairing_open{};
    std::uint16_t pairing_remaining_seconds{};
    std::vector<Sensor> sensors;
    bool queue_running{};
    bool queue_paused{};
    bool queue_loop{};
    bool queue_autostart{};
    std::size_t queue_current_step{};
    std::uint32_t queue_remaining_seconds{};
    std::vector<QueueStep> queue_steps;
    std::vector<AutomationRule> automation_rules;
    bool hybrid_logic_or{};
    bool hub_online{};
    std::uint16_t hub_pan_id{};
    std::uint8_t hub_channel{};
    std::uint16_t sensor_sleep_sec{20};
    std::uint32_t pending_event_count{};
};

struct Command {
    enum class Type {
        set_relay,
        all_relays_off,
        set_mode,
        set_pairing,
        forget_node,
        forget_all_nodes,
        configure_queue,
        control_queue,
        configure_automation,
        set_hybrid_logic,
        start_ota,
        reset_wifi,
        run_link_test,
        set_sensor_sleep
    };

    Type type{};
    std::uint8_t index{};
    bool enabled{};
    OperationMode mode{OperationMode::manual};
    QueueAction queue_action{QueueAction::stop};
    std::uint16_t sleep_seconds{};
    std::vector<QueueStep> queue_steps;
    std::vector<AutomationRule> automation_rules;
};

struct CommandResult {
    bool accepted{};
    std::string message;
};

class GatewayBackend {
public:
    using SnapshotHandler = std::function<void(const GatewaySnapshot &)>;
    virtual ~GatewayBackend() = default;
    virtual GatewaySnapshot snapshot() const = 0;
    virtual CommandResult submit(const Command &command) = 0;
    virtual std::size_t subscribe(SnapshotHandler handler) = 0;
    virtual void unsubscribe(std::size_t subscription) = 0;
};

class MockGateway final : public GatewayBackend {
public:
    MockGateway();
    GatewaySnapshot snapshot() const override;
    CommandResult submit(const Command &command) override;
    std::size_t subscribe(SnapshotHandler handler) override;
    void unsubscribe(std::size_t subscription) override;
    void tick(std::uint32_t elapsed_ms);
    void setSensorsForTest(std::vector<Sensor> sensors);
    void setConnectivityForTest(bool link, bool wifi, bool mqtt, bool hub);

private:
    void publish();
    GatewaySnapshot state_;
    std::uint64_t sensor_phase_ms_{};
    std::uint64_t pairing_phase_ms_{};
    std::uint64_t queue_phase_ms_{};
    std::size_t next_subscription_{1};
    std::vector<std::pair<std::size_t, SnapshotHandler>> subscribers_;
};

}  // namespace hmi
