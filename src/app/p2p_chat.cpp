#include "app/p2p_chat.h"

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "net/raii_socket.h"
#include "protocol/message.hpp"
#include "protocol/protocol_api.h"
#include "utils/p2p_error.h"

namespace messenger::app {

using Clock = std::chrono::steady_clock;

constexpr int ACK_TIMEOUT_SECONDS = 5;
constexpr char KEY_BACKSPACE = 127;

struct PendingAck {
    std::uint32_t id{};
    Clock::time_point deadline;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::optional<PendingAck> pending_ack{};

bool typing_sent = false;

// Буфер текущей строки ввода
std::string input_buffer;

// Сохранённые настройки терминала
termios orig_termios{};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Включение raw‑mode терминала
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        messenger::utils::throw_system_error("tcgetattr");
    }

    termios raw = orig_termios;

    // Отключить канонический режим и echo
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;   // читать минимум 1 символ
    raw.c_cc[VTIME] = 0;  // без таймаута

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        messenger::utils::throw_system_error("tcsetattr");
    }
}

// Выключение raw‑mode и возврат настроек терминала
void disableRawMode() {
    // Без проверки ошибок. Если на выходе "кривой" терминал,
    // то чинится reset/stty sane.
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

// RAII-обёртка функций изменения режима терминала
namespace {
class TerminalRawGuard {
public:
    TerminalRawGuard() {
        enableRawMode();
    }
    ~TerminalRawGuard() {
        disableRawMode();
    }

    TerminalRawGuard(const TerminalRawGuard&) = delete;
    TerminalRawGuard& operator=(const TerminalRawGuard&) = delete;
    TerminalRawGuard(TerminalRawGuard&&) = delete;
    TerminalRawGuard& operator=(TerminalRawGuard&&) = delete;
};
}  // namespace

// Обработчик сигналов завершения
void handleExitSignal([[maybe_unused]] int signal_number) {
    disableRawMode();
    std::exit(1);
}

// Перерисовка строки ввода
void clearInputLine() {  // Стереть текущую строку
    std::cout << "\r\033[K" << std::flush;
}

void redrawInput() {  // Перерисовать вновь строку
    std::cout << "\r> " << input_buffer << "\033[K" << std::flush;
}

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
    using messenger::proto::MsgType;

    switch (msg.type) {
        case MsgType::Text: {
            clearInputLine();
            std::cout << "\n[Собеседник]: " << msg.payload << "\n";
            redrawInput();

            if (!messenger::proto::send_ack(sock_fd, msg.id)) {
                std::cout << "\n[Ошибка: не удалось отправить Ack]\n";
                redrawInput();
            }
            // Возможно логирование в дальнейшем
            // Возможно false и добавить логику обработки, например:
            // повторная отправка Ack или проверка связи Ping/Pong
            // и в случае неуспеха завершение с "Ошибкой связи"
            return true;
        }

        case MsgType::Typing:
            clearInputLine();
            std::cout << "\n[Собеседник печатает...]\n";
            redrawInput();
            return true;

        case MsgType::Ping: {
            if (!messenger::proto::send_pong(sock_fd, msg.id)) {
                clearInputLine();
                std::cout << "\n[Ошибка: не удалось отправить Pong]\n";
                redrawInput();
            }
            return true;  // возможно false / логика обработки в дальнейшем
        }

        case MsgType::Pong:
            clearInputLine();
            std::cout << "\n[Собеседник: получен пакет Pong]\n";
            redrawInput();
            // Здесь позже добавить логику (например, измерения RTT, если
            // понадобится) и логирование
            return true;

        case MsgType::Ack:
            // Обработка Ack происходит в handle_peer отдельно
            return true;

        default:
            clearInputLine();
            std::cout << "\n[Получен пакет с неизвестным типом]\n";
            redrawInput();
            return true;  // возможно false
    }
}

void checkAckTimeout() {
    if (!pending_ack) {
        return;
    }

    const auto now = Clock::now();
    if (now >= pending_ack->deadline) {
        clearInputLine();
        std::cout << "\n[Сообщение msg_id=" << pending_ack->id
                  << " НЕ доставлено (таймаут)]\n";
        redrawInput();  // в дальнейшем логирование
        pending_ack.reset();
    }
}

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

    const bool okey =
        messenger::proto::receive_msg(socket_fd, msg, disconnected);

    if (!okey) {
        clearInputLine();
        std::cout << "\n[Ошибка протокола: получено некорректное сообщение]\n";
        redrawInput();
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
            clearInputLine();
            std::cout << "\n[Сообщение msg_id=" << msg.id << " доставлено]\n";
            redrawInput();
            pending_ack.reset();
            return true;
        }
        // Ack с другим id — игнорируем (или можно логируем)
        return true;
    }

    if (!handleIncomingMessage(socket_fd, msg)) {
        return false;
    }

    return true;
}

bool handle_user(int socket_fd) {
    char key{};
    const auto bytes_read = ::read(STDIN_FILENO, &key, 1);
    if (bytes_read <= 0) {
        std::cout << "\n[Системный EOF или ошибка на stdin. НЕ выходим]\n";
        // логировать
        return true;
    }

    // ====== ВЫХОД ПО ОСОБЫМ КЛАВИШАМ ======
    // Ctrl-D (EOF)
    if (key == '\x04') {
        std::cout << "\nВыход по Ctrl-D\n";
        return false;
    }

    // Esc
    if (key == '\x1B') {
        std::cout << "\nВыход по Esc\n";
        return false;
    }

    // Ctrl-C — обрабатывается через обработчик сигнала SIGINT
    // (handleExitSignal), поэтому здесь символ '\x03' мы не ловим. В raw-mode
    // он обычно не доходит как символ, а сразу превращается в сигнал.

    // ====== ENTER ======
    if (key == '\n' || key == '\r') {
        // Команда выхода /выход или /exit
        if (input_buffer == "/выход" || input_buffer == "/exit") {
            std::cout << "\nОтключаемся...\n";
            return false;
        }

        // Отправка обычного сообщения
        if (!input_buffer.empty()) {
            const std::uint32_t msg_id = generateMessageId();

            if (!messenger::proto::send_text(socket_fd, input_buffer, msg_id)) {
                std::cout
                    << "\n[Ошибка: сообщение не удалось отправить полностью]\n";
                redrawInput();
                // возможо false или логирование / помещение сообщения в очередь
                // отправки
            } else {
                std::cout << "\n[Ожидание подтверждения доставки для msg_id="
                          << msg_id << "]\n";
                // Запуск неблокирующего ожидания Ack: запомнить, что ждём его
                PendingAck ack_state{};
                ack_state.id = msg_id;
                ack_state.deadline =
                    Clock::now() + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
                pending_ack = ack_state;
            }

            input_buffer.clear();
            typing_sent = false;
        } else {
            // Пустой Enter — сбросить посылку Typing
            typing_sent = false;
        }

        redrawInput();
        return true;
    }

    // ====== BACKSPACE ======
    if (key == KEY_BACKSPACE) {
        if (!input_buffer.empty()) {
            input_buffer.pop_back();
            redrawInput();
        }
        return true;
    }

    // ====== Обычный символ ======
    input_buffer.push_back(key);

    // Отправить Typing один раз при начале ввода
    if (!typing_sent) {
        static_cast<void>(messenger::proto::send_typing(socket_fd, 0));
        typing_sent = true;
    }

    redrawInput();
    return true;
}

void chat_loop(messenger::net::Socket sock) {
    const int fd_sock = sock.fd_return();

    // Устанавить обработчики сигналов
    std::signal(SIGINT, handleExitSignal);   // Ctrl-C
    std::signal(SIGTERM, handleExitSignal);  // kill
    std::signal(SIGSEGV, handleExitSignal);  // segmentation fault
    std::signal(SIGABRT, handleExitSignal);  // abort()

    const TerminalRawGuard term_guard;

    std::cout
        << "Чат готов. Печатай сообщение и жми Enter.\n"
        << "Команда выхода: /выход или /exit, а также Ctrl-D или Esc.\n\n";
    redrawInput();

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

        // Проверить после обработки событий, истёк ли таймаут ожидания Ack
        checkAckTimeout();
    }

    std::cout << "\nЧат завершён.\n";
}

}  // namespace messenger::app
