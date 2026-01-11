#include "protocol/protocol_api.h"

#include <cstdint>
#include <string>
#include <vector>

#include "net/net_api.h"
#include "protocol/message.hpp"
#include "protocol/serializer.h"

namespace messenger::proto {

[[nodiscard]]
auto send_text(int socket_fd, const std::string& text,
               std::uint32_t msg_id) -> bool {
    const Message msg{MsgType::Text, msg_id, text};
    const auto bytes = serialize(msg);
    return messenger::net::send_bytes(socket_fd, bytes);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]]
auto send_typing(int socket_fd, std::uint32_t msg_id) -> bool {
    const Message msg{MsgType::Typing, msg_id, ""};
    auto bytes = serialize(msg);
    return net::send_bytes(socket_fd, bytes);
}

[[nodiscard]]
auto send_ping(int socket_fd, std::uint32_t msg_id) -> bool {
    const Message msg{MsgType::Ping, msg_id, std::string{}};
    auto bytes = serialize(msg);
    return messenger::net::send_bytes(socket_fd, bytes);
}

[[nodiscard]]
auto send_pong(int socket_fd, std::uint32_t msg_id) -> bool {
    const Message msg{MsgType::Pong, msg_id, std::string{}};
    auto bytes = serialize(msg);
    return messenger::net::send_bytes(socket_fd, bytes);
}

[[nodiscard]]
auto send_ack(int socket_fd, std::uint32_t msg_id) -> bool {
    const Message msg{MsgType::Ack, msg_id, std::string{}};
    auto bytes = serialize(msg);
    return messenger::net::send_bytes(socket_fd, bytes);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]]
auto receive_msg(int socket_fd, Message& out, bool& disconnected) -> bool {
    std::vector<std::uint8_t> raw;
    const bool okey = net::recv_bytes(socket_fd, raw);

    if (!okey) {
        // Ошибка протокола (неполный заголовок, слишком большой len и т. п.)
        disconnected = false;
        return false;
    }

    if (raw.empty()) {
        // Собеседник корректно отключился
        disconnected = true;
        return true;
    }

    disconnected = false;
    return deserialize(raw, out);
}

}  // namespace messenger::proto
