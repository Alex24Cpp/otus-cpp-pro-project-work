#include "net/client_socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/raii_socket.h"
#include "net/server_socket.h"
#include "utils/p2p_error.h"

namespace messenger::net {

// ---------- Создание клиентского сокета ----------

Socket create_client_socket(std::string_view host, uint16_t port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        utils::throw_system_error("socket");
    }

    // Передача владение в RAII сразу после проверки
    Socket client_socket(socket_fd);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.data(), &server_addr.sin_addr) <= 0) {
        throw std::runtime_error("Недопустимый хост: " + std::string(host));
    }

    std::cout << "Подключение к " << host << ":" << port << "...\n";

    if (connect(client_socket.fd_return(),
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        utils::throw_system_error("connect");
    }

    std::cout << "Подключено.\n";
    return client_socket;
}

}  // namespace messenger::net
