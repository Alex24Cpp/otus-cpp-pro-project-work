#pragma once

#include <cstdint>
#include <vector>

#include "protocol/message.hpp"

namespace messenger::proto {

// Сериализация: Message -> bytes
[[nodiscard]]
auto serialize(const Message& msg) -> std::vector<std::uint8_t>;

// Десериализация: bytes -> Message
// Возвращает true, если буфер корректен и out заполнен.
[[nodiscard]]
auto deserialize(const std::vector<std::uint8_t>& buffer, Message& out) -> bool;

} // namespace messenger::proto
