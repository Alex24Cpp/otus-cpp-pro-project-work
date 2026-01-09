#include "protocol/protocol_api.h"

#include <cstdint>
#include <string>
#include <vector>

#include "net/net_api.h"
#include "protocol/message.hpp"
#include "protocol/serializer.h"

namespace messenger::proto {

[[nodiscard]]
auto send_text(const std::string& text,
               std::uint32_t id) -> std::vector<std::uint8_t> {
    const Message msg{MsgType::Text, id, text};
    return serialize(msg);
}

[[nodiscard]]
auto send_typing(std::uint32_t id) -> std::vector<std::uint8_t> {
    const Message msg{MsgType::Typing, id, std::string{}};
    return serialize(msg);
}

[[nodiscard]]
auto send_ping(std::uint32_t id) -> std::vector<std::uint8_t> {
    const Message msg{MsgType::Ping, id, std::string{}};
    return serialize(msg);
}

[[nodiscard]]
auto send_pong(std::uint32_t id) -> std::vector<std::uint8_t> {
    const Message msg{MsgType::Pong, id, std::string{}};
    return serialize(msg);
}

[[nodiscard]]
auto send_ack(std::uint32_t id) -> std::vector<std::uint8_t> {
    const Message msg{MsgType::Ack, id, std::string{}};
    return serialize(msg);
}

[[nodiscard]]
auto receive_msg(int socket_fd, Message& out, bool& disconnected) -> bool {
    std::vector<std::uint8_t> raw;
    const bool ok = net::recv_bytes(socket_fd, raw);

    if (!ok) {
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
