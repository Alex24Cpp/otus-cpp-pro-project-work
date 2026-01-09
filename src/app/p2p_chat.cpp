#include "app/p2p_chat.h"

#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "net/net_api.h"
#include "net/raii_socket.h"
#include "protocol/message.hpp"
#include "protocol/protocol_api.h"
#include "utils/p2p_error.h"

namespace messenger::app {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int ACK_TIMEOUT_SECONDS = 5;

struct PendingAck {
    std::uint32_t id{};
    Clock::time_point deadline;
};

std::optional<PendingAck> pending_ack{};

[[nodiscard]]
auto generateMessageId() -> std::uint32_t {
    static std::uint32_t next_id = 1;
    if (next_id == std::numeric_limits<std::uint32_t>::max()) {
        next_id = 1;
    }
    return next_id++;
}

[[nodiscard]]
auto handleIncomingMessage(int sock_fd,
                           const messenger::proto::Message& msg) -> bool {
    using messenger::net::send_bytes;
    using messenger::proto::MsgType;
    using messenger::proto::send_ack;

    switch (msg.type) {
        case MsgType::Text: {
            std::cout << "\n[Собеседник]: " << msg.payload << "\n> "
                      << std::flush;
            const auto ack_bytes = send_ack(msg.id);
            if (!send_bytes(sock_fd, ack_bytes)) {
                std::cout
                    << "\n[Ошибка: не удалось отправить Ack]\n";  // Возможно
                                                                  // логирование
                                                                  // в
                                                                  // дальнейшем
                // возможно false и добавить логику обработки, например:
                // повторная отправка Ack или проверка связи Ping/Pong
                // и в случае неуспеха завершение с "Ошибкой связи"
                return true;
            }
            return true;
        }
        case MsgType::Typing:
            std::cout << "\n[Собеседник печатает...]\n> " << std::flush;
            return true;

        case MsgType::Ping: {
            const auto pong_bytes = messenger::proto::send_pong(msg.id);
            if (!send_bytes(sock_fd, pong_bytes)) {
                std::cout << "\n[Ошибка: не удалось отправить Pong]\n";
                return true;  // возможно false / логика обработки в дальнейшем
            }
            return true;
        }

        case MsgType::Pong:
            std::cout << "\n[Собеседник: получен пакет Pong" << msg.id
                      << "]\n> " << std::flush;
            // Здесь позже добавить логику (например, измерения RTT, если
            // понадобится) и логирование
            return true;

        case MsgType::Ack:
            // Обработка Ack происходит в handle_peer отдельно
            return true;

        default:
            std::cout << "\n[Получен пакет с неизвестным типом]\n> "
                      << std::flush;  // Возможно логирование в дальнейшем
            return true;  // возможно false
    }
}

void checkAckTimeout() {
    if (!pending_ack) {
        return;
    }

    const auto now = Clock::now();
    if (now >= pending_ack->deadline) {
        std::cout << "\n[Сообщение msg_id=" << pending_ack->id
                  << " НЕ доставлено (таймаут)]\n> "
                  << std::flush;  // в дальнейшем логирование
        pending_ack.reset();
    }
}

}  // namespace

void wait_for_events(int sock_fd, fd_set& readfds) {
    FD_ZERO(&readfds);
    FD_SET(sock_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    const int max_fd = std::max(sock_fd, STDIN_FILENO);

    const int ret = ::select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
    if (ret < 0) {
        messenger::utils::throw_system_error("select");
    }
}

bool handle_peer(int socket_fd) {
    messenger::proto::Message msg{};
    bool disconnected = false;

    const bool okay =
        messenger::proto::receive_msg(socket_fd, msg, disconnected);

    if (!okay) {
        std::cout << "\n[Ошибка протокола: получено некорректное сообщение]\n> "
                  << std::flush;
        // в дальнейшем логирование и/или логика обработки
        return true;
    }

    if (disconnected) {
        std::cout << "\nСобеседник отключился.\n";
        return false;
    }

    // Обработка Ack: проверка на ожидаемый id
    if (msg.type == messenger::proto::MsgType::Ack) {
        if (pending_ack && msg.id == pending_ack->id) {
            std::cout << "\n[Сообщение msg_id=" << msg.id << " доставлено]\n> "
                      << std::flush;
            pending_ack.reset();
            return true;
        }
        // Ack с другим id — игнорировать (или можно залогировать)
        return true;
    }

    if (!handleIncomingMessage(socket_fd, msg)) {
        return false;
    }

    return true;
}

bool handle_user(int socket_fd) {
    std::string user_input_text;
    if (!std::getline(std::cin, user_input_text)) {
        std::cout << "\nEOF на stdin. Выходим.\n";
        return false;
    }

    if (user_input_text == "/выход") {
        std::cout << "Отключаемся...\n";
        return false;
    }

    const std::uint32_t msg_id = generateMessageId();
    const auto bytes = messenger::proto::send_text(user_input_text, msg_id);

    if (!messenger::net::send_bytes(socket_fd, bytes)) {
        std::cout << "\n[Ошибка: сообщение не удалось отправить полностью]\n";
        return true;  // возможо false или логирование / помещение сообщения в
                      // очередь отправки
    }

    std::cout << "\n[Ожидание подтверждения доставки для msg_id=" << msg_id
              << "...]\n> " << std::flush;

    // Запуск неблокирующего ожидания Ack: запомнить, что ждём его
    PendingAck ack_state{};
    ack_state.id = msg_id;
    ack_state.deadline =
        Clock::now() + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
    pending_ack = ack_state;

    return true;
}

void chat_loop(messenger::net::Socket sock) {
    std::cout << "Чат готов. Печатай сообщение и жми Enter.\n"
              << "Команда выхода: /выход\n\n> " << std::flush;

    const int fd_sock = sock.fd_return();

    while (true) {
        fd_set readfds;
        wait_for_events(fd_sock, readfds);

        if (FD_ISSET(fd_sock, &readfds)) {
            if (!handle_peer(fd_sock)) {
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!handle_user(fd_sock)) {
                break;
            }
        }

        // После обработки событий провка, истёк ли таймаут ожидания Ack
        checkAckTimeout();
    }
}

}  // namespace messenger::app
