#include <iostream>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <memory>
#include <span>
#include <algorithm>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

#include "p2p_error.h"


// ---------- Утилиты ----------


// RAII-обёртка для сокета
class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {
        if (fd_ < 0) throw std::invalid_argument("Socket: недопустимый файловый дескриптор");
    }

    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int fd() const { return fd_; }

private:
    int fd_;
};

// ---------- Создание серверного сокета ----------

Socket create_server_socket(uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw_system_error("socket");

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        throw_system_error("setsockopt");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw_system_error("bind");

    if (listen(sock, 1) < 0)
        throw_system_error("listen");

    std::cout << "Ожидание подключения на порту " << port << "...\n";

    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(sock, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0)
        throw_system_error("accept");

    std::cout << "Клиент подключен: "
              << inet_ntoa(client_addr.sin_addr) << ":"
              << ntohs(client_addr.sin_port) << "\n";

    ::close(sock);
    return Socket(client_fd);
}

// ---------- Создание клиентского сокета ----------

Socket create_client_socket(std::string_view host, uint16_t port) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) throw_system_error("socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.data(), &addr.sin_addr) <= 0)
        throw std::runtime_error("Недопустимый хост: " + std::string(host));

    std::cout << "Подключение к " << host << ":" << port << "...\n";

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw_system_error("connect");

    std::cout << "Подключено.\n";
    return Socket(sock);
}

// ---------- Основной цикл чата ----------

void chat_loop(Socket sock) {
    std::vector<char> buffer(4096);

    std::cout << "Чат готов. Печатай сообщение и жми Enter.\n"
              << "Команда выхода: /выход\n\n";

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock.fd(), &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int max_fd = std::max(sock.fd(), STDIN_FILENO);

        int ret = ::select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
        if (ret < 0) throw_system_error("select");

        // данные от собеседника
        if (FD_ISSET(sock.fd(), &readfds)) {
            ssize_t n = ::recv(sock.fd(), buffer.data(), buffer.size(), 0);
            if (n < 0) throw_system_error("recv");
            if (n == 0) {
                std::cout << "\nСобеседник отключился.\n";
                break;
            }

            std::string_view msg(buffer.data(), static_cast<size_t>(n));
            std::cout << "\n[Собеседник]: " << msg << "\n> " << std::flush;
        }

        // ввод пользователя
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                std::cout << "\nEOF на stdin. Выходим.\n";
                break;
            }

            if (line == "/выход") {
                std::cout << "Отключаемся...\n";
                break;
            }

            ssize_t sent = ::send(sock.fd(), line.data(), line.size(), 0);
            if (sent < 0) throw_system_error("send");

            std::cout << "> " << std::flush;
        }
    }
}

// ---------- main() ----------

int main(int argc, char* argv[]) try {
    if (argc < 2) {
        std::cerr << "Использование:\n"
                  << "  " << argv[0] << " сервер <порт>\n"
                  << "  " << argv[0] << " клиент требуется <хост> <порт>\n";
        return EXIT_FAILURE;
    }

    std::string_view mode{argv[1]};

    if (mode == "сервер") {
        if (argc != 3)
            throw std::invalid_argument("сервер: требуется порт");

        uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
        auto sock = create_server_socket(port);
        chat_loop(std::move(sock));

    } else if (mode == "клиент") {
        if (argc != 4)
            throw std::invalid_argument("клиент: требуется хост и порт");

        std::string_view host{argv[2]};
        uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));

        auto sock = create_client_socket(host, port);
        chat_loop(std::move(sock));

    } else {
        throw std::invalid_argument("Неизвестный режим: " + std::string(mode));
    }

    return EXIT_SUCCESS;

} catch (const std::exception& ex) {
    std::cerr << "Фатальная ошибка: " << ex.what() << "\n";
    return EXIT_FAILURE;
}
