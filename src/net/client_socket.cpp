#include "net/client_socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

#include "net/raii_socket.h"
#include "net/server_socket.h"
#include "utils/p2p_error.h"

namespace messenger::net {

// ---------- Создание клиентского сокета ----------

Socket create_client_socket(std::string_view host, uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) utils::throw_system_error("socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.data(), &addr.sin_addr) <= 0)
        throw std::runtime_error("Недопустимый хост: " + std::string(host));

    std::cout << "Подключение к " << host << ":" << port << "...\n";

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        utils::throw_system_error("connect");

    std::cout << "Подключено.\n";
    return Socket(sock);
}

}  // namespace messenger::net