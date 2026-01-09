#include "protocol/serializer.h"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "protocol/message.hpp"

namespace messenger::proto {

namespace {

// [type(1)][id(4)][len(4)][payload]
constexpr std::size_t header_size = 1 + 4 + 4;

[[nodiscard]]
auto msg_type_valid(MsgType type) -> bool {
    switch (type) {
        case MsgType::Text:
        case MsgType::Typing:
        case MsgType::Ack:
        case MsgType::Ping:
        case MsgType::Pong:
            return true;
        default:
            return false;
    }
}

}  // namespace

[[nodiscard]]
auto serialize(const Message& msg) -> std::vector<std::uint8_t> {
    const std::uint32_t payload_size =
        static_cast<std::uint32_t>(msg.payload.size());

    const std::uint32_t id_net = htonl(msg.id);
    const std::uint32_t payload_size_net = htonl(payload_size);

    const std::array<std::uint8_t, 4> id_bytes =
        std::bit_cast<std::array<std::uint8_t, 4>>(id_net);

    const std::array<std::uint8_t, 4> len_bytes =
        std::bit_cast<std::array<std::uint8_t, 4>>(payload_size_net);

    std::vector<std::uint8_t> buffer;
    buffer.reserve(header_size + payload_size);

    // Тип
    buffer.push_back(static_cast<std::uint8_t>(msg.type));

    // ID (4 байта)
    buffer.insert(buffer.end(), id_bytes.begin(), id_bytes.end());

    // Длина (4 байта)
    buffer.insert(buffer.end(), len_bytes.begin(), len_bytes.end());

    // Полезная нагрузка
    buffer.insert(buffer.end(), msg.payload.begin(), msg.payload.end());

    return buffer;
}

[[nodiscard]]
auto deserialize(const std::vector<std::uint8_t>& buffer,
                 Message& out) -> bool {
    if (buffer.size() < header_size) {
        return false;
    }

    const auto type = static_cast<MsgType>(buffer.front());
    if (!msg_type_valid(type)) {
        return false;
    }

    // ID
    std::array<std::uint8_t, 4> id_bytes{};
    std::copy_n(buffer.begin() + 1, 4, id_bytes.begin());
    const std::uint32_t id_net = std::bit_cast<std::uint32_t>(id_bytes);
    const std::uint32_t id = ntohl(id_net);

    // Длина payload
    std::array<std::uint8_t, 4> len_bytes{};
    std::copy_n(buffer.begin() + 1 + 4, 4, len_bytes.begin());
    const std::uint32_t payload_size_net =
        std::bit_cast<std::uint32_t>(len_bytes);
    const std::uint32_t payload_size = ntohl(payload_size_net);

    if (buffer.size() != header_size + payload_size) {
        return false;
    }

    out.type = type;
    out.id = id;

    const auto* payload_bytes = buffer.data() + header_size;

    out.payload.resize(payload_size);
    std::memcpy(out.payload.data(), payload_bytes, payload_size);

    return true;
}

}  // namespace messenger::proto
