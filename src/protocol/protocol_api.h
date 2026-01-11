#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/message.hpp"

namespace messenger::proto {

// Формирование байтов для отправки

[[nodiscard]]
auto send_text(int socket_fd, const std::string& text, std::uint32_t msg_id) -> bool;

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]]
auto send_typing(int socket_fd, std::uint32_t msg_id) -> bool;

[[nodiscard]]
auto send_ping(int socket_fd, std::uint32_t msg_id) -> bool;

[[nodiscard]]
auto send_pong(int socket_fd, std::uint32_t msg_id) -> bool;

[[nodiscard]]
auto send_ack(int socket_fd, std::uint32_t msg_id) -> bool;
// NOLINTEND(bugprone-easily-swappable-parameters)

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

