#pragma once

#include <cstdint>
#include <string>

namespace messenger::proto {

enum class MsgType : std::uint8_t {
    Text = 0x01,
    Typing = 0x02,
    Ack = 0x03,
    Ping = 0x04,
    Pong = 0x05
};

struct Message {
    MsgType type;
    std::uint32_t id;
    std::string payload;
};

}  // namespace messenger::proto
