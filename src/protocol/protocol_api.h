#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/message.hpp"

namespace messenger::proto {

// Формирование байтов для отправки

[[nodiscard]]
auto send_text(const std::string& text, std::uint32_t id) -> std::vector<std::uint8_t>;

[[nodiscard]]
auto send_typing(std::uint32_t id) -> std::vector<std::uint8_t>;

[[nodiscard]]
auto send_ping(std::uint32_t id) -> std::vector<std::uint8_t>;

[[nodiscard]]
auto send_pong(std::uint32_t id) -> std::vector<std::uint8_t>;

[[nodiscard]]
auto send_ack(std::uint32_t id) -> std::vector<std::uint8_t>;

// Приём одного сообщения с сокета.
//
// Возвращает:
//  - false → ошибка протокола (некорректный фрейм).
//  - true  → либо корректное сообщение, либо отключение собеседника.
//
// Параметры:
//  - out           — заполняется, если получено корректное сообщение.
//  - disconnected  — устанавливается в true, если собеседник отключился ДО
//                    нового сообщения.
[[nodiscard]]
auto receive_msg(int socket_fd, Message& out, bool& disconnected) -> bool;

} // namespace messenger::proto

