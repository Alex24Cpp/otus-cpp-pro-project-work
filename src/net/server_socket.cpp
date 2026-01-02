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
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        utils::throw_system_error("socket");
    }

    int opt = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        utils::throw_system_error("setsockopt");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        utils::throw_system_error("bind");
    }

    if (listen(sock, 1) < 0) {
        utils::throw_system_error("listen");
    }

    std::cout << "Ожидание подключения на порту " << port << "...\n";

    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    const int client_fd =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        accept(sock, (sockaddr*)&client_addr, &len);
    if (client_fd < 0) {
        utils::throw_system_error("accept");
    }

    std::cout << "Клиент подключен: " << inet_ntoa(client_addr.sin_addr) << ":"
              << ntohs(client_addr.sin_port) << "\n";

    ::close(sock);
    return Socket(client_fd);
}

}  // namespace messenger::net
