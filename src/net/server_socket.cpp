#include "net/server_socket.h"

#include <arpa/inet.h>  // inet_pton, inet_ntop
#include <bits/socket.h>
#include <netinet/in.h>  // sockaddr_in
#include <sys/socket.h>  // socket, accept, bind, recv, send
#include <unistd.h>      // close

#include <cstdint>
#include <iostream>

#include "net/raii_socket.h"
#include "utils/p2p_error.h"

namespace messenger::net {

// ---------- Создание серверного сокета ----------

Socket create_server_socket(uint16_t port) {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        utils::throw_system_error("socket");
    }

    // Передача владение в RAII сразу после проверки
    const Socket server_socket(server_fd);

    int option_value = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(server_socket.fd_return(), SOL_SOCKET, SO_REUSEADDR,
                   &option_value, sizeof(option_value)) < 0) {
        utils::throw_system_error("setsockopt");
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    if (bind(server_socket.fd_return(), (sockaddr*)&server_addr,
             sizeof(server_addr)) < 0) {
        utils::throw_system_error("bind");
    }

    if (listen(server_socket.fd_return(), 1) < 0) {
        utils::throw_system_error("listen");
    }

    std::cout << "Ожидание подключения на порту " << port << "...\n";

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        accept(server_socket.fd_return(), (sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        utils::throw_system_error("accept");
    }

    std::cout << "Клиент подключен: " << inet_ntoa(client_addr.sin_addr) << ":"
              << ntohs(client_addr.sin_port) << "\n";

    return Socket(client_fd);
}

}  // namespace messenger::net
