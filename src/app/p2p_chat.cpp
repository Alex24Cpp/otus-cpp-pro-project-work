#include "app/p2p_chat.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "net/raii_socket.h"
#include "utils/p2p_error.h"

namespace {
constexpr std::size_t BUFFER_SIZE = 4096;
}

namespace messenger::app {

// ---------- Функция ожидания событий ----------

void wait_for_events(int sock_fd, fd_set& readfds) {
    FD_ZERO(&readfds);
    FD_SET(sock_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    const int max_fd = std::max(sock_fd, STDIN_FILENO);

    const int ret = ::select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
    if (ret < 0) {
        utils::throw_system_error("select");
    }
}

// ---------- Обработка сообщения от собеседника ----------

bool handle_peer(int sock_fd, std::vector<char>& buffer) {
    const ssize_t number = ::recv(sock_fd, buffer.data(), buffer.size(), 0);
    if (number < 0) {
        utils::throw_system_error("recv");
    }
    if (number == 0) {
        std::cout << "\nСобеседник отключился.\n";
        return false;
    }

    const std::string_view msg(buffer.data(), static_cast<size_t>(number));
    std::cout << "\n[Собеседник]: " << msg << "\n> " << std::flush;
    return true;
}

// ---------- Обработка пользовательского ввода ----------

bool handle_user(int sock_fd) {
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cout << "\nEOF на stdin. Выходим.\n";
        return false;
    }

    if (line == "/выход") {
        std::cout << "Отключаемся...\n";
        return false;
    }

    const ssize_t sent = ::send(sock_fd, line.data(), line.size(), 0);
    if (sent < 0) {
        utils::throw_system_error("send");
    }

    std::cout << "> " << std::flush;
    return true;
}

// ---------- Основной цикл чата ----------

void chat_loop(messenger::net::Socket sock) {
    std::vector<char> buffer(BUFFER_SIZE);

    std::cout << "Чат готов. Печатай сообщение и жми Enter.\n"
              << "Команда выхода: /выход\n\n";

    while (true) {
        fd_set readfds;
        wait_for_events(sock.fd_return(), readfds);

        if (FD_ISSET(sock.fd_return(), &readfds)) {
            if (!handle_peer(sock.fd_return(), buffer)) {
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!handle_user(sock.fd_return())) {
                break;
            }
        }
    }
}

}  // namespace messenger::app
