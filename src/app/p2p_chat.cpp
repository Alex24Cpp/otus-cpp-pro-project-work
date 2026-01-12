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
#include <unordered_set>

#include "net/raii_socket.h"
#include "protocol/message.hpp"
#include "protocol/protocol_api.h"
#include "utils/p2p_error.h"

namespace messenger::app {

using Clock = std::chrono::steady_clock;

constexpr int ACK_TIMEOUT_SECONDS = 5;
constexpr char KEY_BACKSPACE = 127;

// Отдельные параметры для ACK и Ping/Pong
constexpr int MAX_MESSAGE_RETRIES = 3;

constexpr int PING_INTERVAL_SECONDS = 10;
constexpr int PING_TIMEOUT_SECONDS = 3;
constexpr int MAX_PING_RETRIES = 3;

struct PendingAck {
    std::uint32_t id{};
    Clock::time_point deadline;
    int retry_count{};
    std::string last_payload;
    bool ping_for_ack_requested{false};
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::optional<PendingAck> pending_ack{};

bool typing_sent = false;

// Буфер текущей строки ввода
std::string input_buffer;

// Сохранённые настройки терминала
termios orig_termios{};

// Set id входящих сообщений для дедупликации
std::unordered_set<std::uint32_t> seen_message_ids{};

// Переменные Ping/Pong‑watchdog'а
Clock::time_point last_ping_time = Clock::now();
Clock::time_point last_pong_time = Clock::now();
int ping_retry_count = 0;

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
auto isDuplicate(std::uint32_t msg_id) -> bool {
    return seen_message_ids.find(msg_id) != seen_message_ids.end();
}

void rememberMessageId(std::uint32_t msg_id) {
    seen_message_ids.insert(msg_id);
    // Ограничитель размера для защиты от бесконечного роста
    if (seen_message_ids.size() > 1024U) {
        seen_message_ids.clear();
    }
}

[[nodiscard]]
auto handleIncomingMessage(int sock_fd,
                           const messenger::proto::Message& msg) -> bool {
    using messenger::proto::MsgType;

    switch (msg.type) {
        case MsgType::Text: {
            // Дедупликация: если msg_id был, не показывать повторно
            if (isDuplicate(msg.id)) {
                if (!messenger::proto::send_ack(sock_fd, msg.id)) {
                    clearInputLine();
                    std::cout
                        << "\n[Ошибка: не удалось повторно отправить Ack]\n";
                    redrawInput();
                }
                return true;
            }

            rememberMessageId(msg.id);

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
            // Pong подтверждает, что соединение живо, сбрасить watchdog
            last_pong_time = Clock::now();
            ping_retry_count = 0;

            // clearInputLine();
            // std::cout << "\n[Собеседник: получен пакет Pong]\n";
            // redrawInput();
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

void checkAckTimeout(int socket_fd) {
    if (!pending_ack) {
        return;
    }

    const auto now = Clock::now();
    if (now < pending_ack->deadline) {
        return;
    }

    // ===== 1. Обычные ретраи до MAX_MESSAGE_RETRIES - 1 =====
    if (pending_ack->retry_count < MAX_MESSAGE_RETRIES - 1) {
        if (!messenger::proto::send_text(socket_fd, pending_ack->last_payload,
                                         pending_ack->id)) {
            clearInputLine();
            std::cout
                << "\n[Ошибка: сообщение не удалось повторно отправить]\n";
            redrawInput();
            pending_ack.reset();
            return;
        }

        pending_ack->retry_count += 1;
        pending_ack->deadline = now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);

        clearInputLine();
        std::cout << "\n[Повторная отправка msg_id=" << pending_ack->id
                  << ", попытка " << pending_ack->retry_count << "]\n";
        redrawInput();
        return;
    }

    // ===== 2. Инициировать Ping/Pong-проверку перед последним ретраем =====
    if (!pending_ack->ping_for_ack_requested) {
        // Форсировать отправку Ping в ближайшем цикле watchdog'а
        last_ping_time = now - std::chrono::seconds(PING_INTERVAL_SECONDS);
        ping_retry_count = 0;

        pending_ack->ping_for_ack_requested = true;
        pending_ack->deadline = now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
        return;
    }

    // ===== 3. После успешного Ping/Pong — выполнить последний ретрай =====
    if (pending_ack->retry_count == MAX_MESSAGE_RETRIES - 1) {
        if (!messenger::proto::send_text(socket_fd, pending_ack->last_payload,
                                         pending_ack->id)) {
            clearInputLine();
            std::cout
                << "\n[Ошибка: сообщение не удалось отправить повторно]\n";
            redrawInput();
            pending_ack.reset();
            return;
        }

        pending_ack->retry_count += 1;  // retry_count == MAX_MESSAGE_RETRIES
        pending_ack->deadline = now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);

        clearInputLine();
        std::cout << "\n[Последняя попытка отправки msg_id=" << pending_ack->id
                  << "]\n";
        redrawInput();
        return;
    }

    // ===== 4. В этой точке:
    //  - обычные ретраи исчерпаны,
    //  - Ping/Pong-проверка уже инициирована,
    //  - последний ретрай выполнен,
    //  - checkPingWatchdog() не заявил о потере соединения,
    //  - но ACK так и не пришёл.
    clearInputLine();
    std::cout << "\n[Сообщение msg_id=" << pending_ack->id
              << " НЕ доставлено (таймаут)]\n";
    redrawInput();  // в дальнейшем логирование

    pending_ack.reset();
}

[[nodiscard]]
auto sendPing(int socket_fd) -> bool {
    // msg_id для Ping не нужен, использовать 0
    return messenger::proto::send_ping(socket_fd, 0);
}

[[nodiscard]]
auto checkPingWatchdog(int socket_fd) -> bool {
    const auto now = Clock::now();

    // Отправлять Ping периодически, даже если пользователь молчит
    if (now - last_ping_time >= std::chrono::seconds(PING_INTERVAL_SECONDS) &&
        ping_retry_count < MAX_PING_RETRIES) {
        if (!sendPing(socket_fd)) {
            clearInputLine();
            std::cout << "\n[Ошибка: не удалось отправить Ping]\n";
            redrawInput();
            return false;
        }
        last_ping_time = now;
        if (ping_retry_count == 0) {
            last_pong_time = now;
        }
        ping_retry_count += 1;
    }

    // Если после нескольких Ping так и не пришёл Pong — соединение считать
    // потерянным
    if (ping_retry_count > 0 &&
        now - last_pong_time > std::chrono::seconds(PING_TIMEOUT_SECONDS) &&
        ping_retry_count >= MAX_PING_RETRIES) {
        clearInputLine();
        std::cout << "\n[Ошибка: соединение потеряно (нет Pong)]\n";
        redrawInput();
        return false;
    }

    return true;
}

void wait_for_events(int sock_fd, fd_set& readfds) {
    FD_ZERO(&readfds);
    FD_SET(sock_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    const int max_fd = std::max(sock_fd, STDIN_FILENO);

    // Таймаут, чтобы периодически будиться для Ping/Ack‑таймеров
    timeval t_v{};
    t_v.tv_sec = 0;
    t_v.tv_usec = 500000;  // 500 мс

    const int ret = ::select(max_fd + 1, &readfds, nullptr, nullptr, &t_v);
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
        // Ack с другим id — игнорируем (или можно логировать)
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
                ack_state.retry_count = 0;
                ack_state.last_payload = input_buffer;
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

    std::cout << "Чат готов. Печатай сообщение и жми Enter.\n"
              << "Команда выхода: /выход или /exit, а также Ctrl-D.\n\n";
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
        checkAckTimeout(fd_sock);

        // Проверка связи через Ping/Pong‑watchdog
        if (!checkPingWatchdog(fd_sock)) {
            break;
        }
    }

    std::cout << "\nЧат завершён.\n";
}

}  // namespace messenger::app
