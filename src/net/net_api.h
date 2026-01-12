#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace messenger::net {

// Максимальный размер полезной нагрузки (1 МБ)
using MaxPayloadSize = std::integral_constant<std::size_t, 1024U * 1024U>;

// Отправка всех байтов.
// Возвращает true, если все байты были отправлены.
// При системной ошибке бросает исключение
[[nodiscard]]
auto send_bytes(int socket_fd, const std::vector<std::uint8_t>& data) -> bool;

// Приём одного фрейма [type][id][len][payload].
// out очищается в начале.
// Семантика:
//  - возвращает false  → ошибка протокола (неполный заголовок, слишком большой len).
//  - возвращает true и out пустой → собеседник отключился ДО заголовка.
//  - возвращает true и out непустой → заголовок прочитан, payload может быть частичным
//    при обрыве соединения; протокол решает, валидно ли сообщение.
// При системной ошибке бросает исключение
[[nodiscard]]
auto recv_bytes(int socket_fd, std::vector<std::uint8_t>& out) -> bool;

} // namespace messenger::net
