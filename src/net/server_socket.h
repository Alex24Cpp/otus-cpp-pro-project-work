#pragma once

#include <cstdint>

#include "net/raii_socket.h"

namespace messenger::net {

Socket create_server_socket(uint16_t port);

} // namespace messenger::net