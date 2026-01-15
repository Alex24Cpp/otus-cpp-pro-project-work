#include "net/net_api.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "utils/p2p_error.h"

namespace messenger::net {

namespace {

constexpr std::size_t HEADER_SIZE = 1 + 4 + 4;

[[nodiscard]]
auto recv_some(int socket_fd, std::vector<std::uint8_t>& buffer,
               std::size_t offset, std::size_t bytes_to_read) -> std::size_t {
    std::size_t total_received = 0;

    while (total_received < bytes_to_read) {
        const auto ret = ::recv(socket_fd,
                                buffer.data() + static_cast<std::ptrdiff_t>(
                                                    offset + total_received),
                                bytes_to_read - total_received, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            utils::throw_system_error("recv");
        }

        if (ret == 0) {
            // Собеседник закрыл соединение
            break;
        }

        total_received += static_cast<std::size_t>(ret);
    }

    return total_received;
}

}  // namespace

[[nodiscard]]
auto send_bytes(int socket_fd, const std::vector<std::uint8_t>& data) -> bool {
    std::size_t total_sent = 0;
    const std::size_t total_size = data.size();

    while (total_sent < total_size) {
        const auto* chunk_ptr =
            data.data() + static_cast<std::ptrdiff_t>(total_sent);
        const std::size_t chunk_size = total_size - total_sent;

        ssize_t ret{};
        ret = ::send(socket_fd, chunk_ptr, chunk_size, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            utils::throw_system_error("send");
        }

        if (ret == 0) {
            // Соединение закрыто или недоступно
            break;
        }

        total_sent += static_cast<std::size_t>(ret);
    }

    return total_sent == total_size;
}

[[nodiscard]]
auto recv_bytes(int socket_fd, std::vector<std::uint8_t>& out) -> bool {
    out.clear();

    std::vector<std::uint8_t> header(
        HEADER_SIZE);  // вектор из HEADER_SIZE элементов
    std::size_t received_header{0};

    while (received_header < HEADER_SIZE) {
        const auto ret =
            ::recv(socket_fd,
                   header.data() + static_cast<std::ptrdiff_t>(received_header),
                   HEADER_SIZE - received_header, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            utils::throw_system_error("recv");
        }

        if (ret == 0) {
            if (received_header == 0) {
                // Собеседник корректно закрыл соединение ДО нового сообщения
                return true;  // out пустой
            }

            // Обрыв в середине заголовка — ошибка протокола
            out.clear();
            return false;
        }

        received_header += static_cast<std::size_t>(ret);
    }

    // Заголовок прочитан полностью, выделить длину нагрузки
    const auto len_bytes_begin = header.begin() + 1 + 4;
    std::array<std::uint8_t, 4> len_bytes{};
    std::copy_n(len_bytes_begin, 4, len_bytes.begin());

    const std::uint32_t len_net = std::bit_cast<std::uint32_t>(len_bytes);
    const std::uint32_t payload_size = ntohl(len_net);

    if (payload_size > MaxPayloadSize::value) {
        // payload слишком большой — ошибка протокола
        out.clear();
        return false;
    }

    out.resize(HEADER_SIZE + static_cast<std::size_t>(payload_size));
    std::copy(header.begin(), header.end(), out.begin());

    if (payload_size == 0) {
        return true;
    }

    const std::size_t bytes_to_read = static_cast<std::size_t>(payload_size);
    const std::size_t received_payload =
        recv_some(socket_fd, out, HEADER_SIZE, bytes_to_read);

    // Если payload неполный (обрыв соединения), вернуть true и
    // частичный фрейм — решение валидно ли сообщение за протоколом
    const std::size_t actual_size = HEADER_SIZE + received_payload;
    out.resize(actual_size);

    return true;
}

}  // namespace messenger::net
