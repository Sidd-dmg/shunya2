#pragma once

#include <cstdint>

namespace hmi {

class MockGateway;
void attach_ui_backend(MockGateway &gateway);
std::uint32_t ui_backend_command_count(std::uint8_t type);
void reset_ui_backend_command_counts();

}  // namespace hmi
