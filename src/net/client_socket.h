#pragma once

#include <cstdint>
#include <string_view>

#include "net/raii_socket.h"

namespace messenger::net {

Socket create_client_socket(std::string_view host, uint16_t port);

} // namespace messenger::net