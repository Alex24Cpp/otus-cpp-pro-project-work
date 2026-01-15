#include "app/p2p_chat.h"

// NOLINTBEGIN(modernize-deprecated-headers)
#include <signal.h>
// NOLINTEND(modernize-deprecated-headers)
#include <sys/select.h>
// NOLINTBEGIN(misc-include-cleaner)
#include <sys/time.h>  // timeval
// NOLINTEND(misc-include-cleaner)
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// Макс. количество хранимых id полученных сообщений
// При превышении старые идентификаторы удаляются для ограничения роста памяти
constexpr std::size_t MAX_SEEN_MESSAGE_IDS = 1024U;

// Лимит на количество строк истории чата
constexpr std::size_t MAX_HISTORY_LINES = 10000U;

// Лимит на размер очереди недоставленных сообщений
constexpr std::size_t MAX_UNDELIVERED_MESSAGES = 1000U;

// Маска для выделения двух старших битов UTF‑8 байта
constexpr unsigned char UTF8_LEAD_MASK = 0xC0U;

// Значение двух старших битов для continuation‑byte (10xxxxxx)
constexpr unsigned char UTF8_CONTINUATION_VALUE = 0x80U;

// Интервал ожидания select() в микросекундах (500 мс)
constexpr int SELECT_TIMEOUT_USEC = 500000;

struct PendingAck {
    std::uint32_t id{};
    Clock::time_point deadline;
    int retry_count{};
    std::string last_payload;
    bool ping_for_ack_requested{false};
};

struct OutgoingMessage {
    std::uint32_t message_id{};
    std::string payload;
    bool delivered{};
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::unordered_map<std::uint32_t, PendingAck> pending_acks{};

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

std::vector<OutgoingMessage> undelivered_messages{};

// Для ведения истории сообщений
std::vector<std::string> chat_history{};
const std::string history_file_path = "chat_history.txt";

// Флаг завершения из обработчика сигналов
std::atomic<bool> shutdown_requested = false;
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
    shutdown_requested.store(true, std::memory_order_relaxed);
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
    if (seen_message_ids.size() > MAX_SEEN_MESSAGE_IDS) {
        seen_message_ids.clear();
    }
}

// Удалять UTF‑8 символы
void eraseLastUtf8Char(std::string& text) {
    if (text.empty()) {
        return;
    }

    std::size_t index = text.size() - 1;

    // UTF‑8 continuation byte: 10xxxxxx
    while (index > 0 && (static_cast<unsigned char>(text[index]) &
                         UTF8_LEAD_MASK) == UTF8_CONTINUATION_VALUE) {
        --index;
    }

    text.erase(index);
}

void resendMessage(int socket_fd, OutgoingMessage& outgoing_message) {
    const std::uint32_t new_message_id = generateMessageId();

    if (!messenger::proto::send_text(socket_fd, outgoing_message.payload,
                                     new_message_id)) {
        std::cout << "\n[Ошибка: не удалось повторно отправить сообщение]\n";
        return;
    }

    outgoing_message.message_id = new_message_id;
    outgoing_message.delivered = false;

    PendingAck ack_state{};
    ack_state.id = new_message_id;
    ack_state.deadline =
        Clock::now() + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
    ack_state.retry_count = 0;
    ack_state.last_payload = outgoing_message.payload;
    pending_acks[new_message_id] = ack_state;

    std::cout << "\n[Повторная отправка msg_id=" << new_message_id << "]\n";
}

// Обработка команды /повтор
void handleRepeatCommand(int socket_fd, const std::string& command_text) {
    if (command_text == "/повтор") {
        std::cout << "\nНедоставленные сообщения:\n";
        for (const auto& outgoing_message : undelivered_messages) {
            if (!outgoing_message.delivered) {
                std::cout << "id=" << outgoing_message.message_id << ": "
                          << outgoing_message.payload << '\n';
            }
        }
        return;
    }

    const std::size_t space_pos = command_text.find(' ');
    if (space_pos == std::string::npos ||
        space_pos + 1 >= command_text.size()) {
        std::cout << "\n[Формат: /повтор <id>]\n";
        return;
    }

    const std::string id_text = command_text.substr(space_pos + 1);
    std::uint32_t message_id_value{};
    try {
        message_id_value = static_cast<std::uint32_t>(std::stoul(id_text));
    } catch (const std::exception&) {
        std::cout << "\n[Некорректный id сообщения]\n";
        return;
    }

    for (auto& outgoing_message : undelivered_messages) {
        if (outgoing_message.message_id == message_id_value &&
            !outgoing_message.delivered) {
            resendMessage(socket_fd, outgoing_message);
            return;
        }
    }

    std::cout << "\n[Сообщение с id=" << message_id_value
              << " не найдено среди недоставленных]\n";
}

void appendToHistoryFile(const std::string& history_line) {
    std::ofstream history_file(history_file_path, std::ios::app);
    if (!history_file) {
        return;
    }
    history_file << history_line << '\n';
}

void loadHistoryFromFile() {
    std::ifstream history_file(history_file_path);
    if (!history_file) {
        return;
    }

    std::string history_line;
    while (std::getline(history_file, history_line)) {
        chat_history.push_back(history_line);
    }
}

void showHistory() {
    std::cout << "\nИстория сообщений:\n";
    for (const auto& history_line : chat_history) {
        std::cout << history_line << '\n';
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

            const std::string history_line = "[Собеседник]: " + msg.payload;
            chat_history.push_back(history_line);
            appendToHistoryFile(history_line);
            if (chat_history.size() > MAX_HISTORY_LINES) {
                chat_history.erase(chat_history.begin());
            }

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
    const auto now = Clock::now();
    std::vector<std::uint32_t> remove_ids;

    for (auto& pair_item : pending_acks) {
        const std::uint32_t msg_id = pair_item.first;
        PendingAck& ack_state = pair_item.second;

        if (now < ack_state.deadline) {
            continue;
        }

        // ===== 1. Обычные ретраи до MAX_MESSAGE_RETRIES - 1 =====
        if (ack_state.retry_count < MAX_MESSAGE_RETRIES - 1) {
            if (!messenger::proto::send_text(socket_fd, ack_state.last_payload,
                                             ack_state.id)) {
                std::cout
                    << "\n[Ошибка: сообщение не удалось повторно отправить]\n";
                remove_ids.push_back(msg_id);
                continue;
            }

            ack_state.retry_count += 1;
            ack_state.deadline =
                now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);

            std::cout << "\n[Повторная отправка msg_id=" << ack_state.id
                      << ", попытка " << ack_state.retry_count << "]\n";
            continue;
        }

        // ===== 2. Инициировать Ping/Pong-проверку перед последним ретраем
        // =====
        if (!ack_state.ping_for_ack_requested) {
            // Форсировать отправку Ping в ближайшем цикле watchdog'а
            last_ping_time = now - std::chrono::seconds(PING_INTERVAL_SECONDS);
            ping_retry_count = 0;

            ack_state.ping_for_ack_requested = true;
            ack_state.deadline =
                now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
            continue;
        }

        // ===== 3. После успешного Ping/Pong — выполнить последний ретрай =====
        if (ack_state.retry_count == MAX_MESSAGE_RETRIES - 1) {
            if (!messenger::proto::send_text(socket_fd, ack_state.last_payload,
                                             ack_state.id)) {
                std::cout
                    << "\n[Ошибка: сообщение не удалось отправить повторно]\n";
                remove_ids.push_back(msg_id);
                continue;
            }

            ack_state.retry_count += 1;  // retry_count == MAX_MESSAGE_RETRIES
            ack_state.deadline =
                now + std::chrono::seconds(ACK_TIMEOUT_SECONDS);

            std::cout << "\n[Последняя попытка отправки msg_id=" << ack_state.id
                      << "]\n";
            continue;
        }

        // ===== 4. В этой точке:
        //  - обычные ретраи исчерпаны,
        //  - Ping/Pong-проверка уже инициирована,
        //  - последний ретрай выполнен,
        //  - checkPingWatchdog() не заявил о потере соединения,
        //  - но ACK так и не пришёл.
        std::cout << "\n[Сообщение msg_id=" << ack_state.id
                  << " НЕ доставлено (таймаут)]\n";  // в дальнейшем логирование

        for (auto& outgoing_message : undelivered_messages) {
            if (outgoing_message.message_id == ack_state.id) {
                outgoing_message.delivered = false;
                break;
            }
        }

        remove_ids.push_back(msg_id);
    }

    for (std::uint32_t id_value : remove_ids) {
        pending_acks.erase(id_value);
    }
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
            std::cout << "\n[Ошибка: не удалось отправить Ping]\n";
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
        std::cout << "\n[Ошибка: соединение потеряно (нет Pong)]\n";
        return false;
    }

    return true;
}

void wait_for_events(int sock_fd, fd_set& readfds) {
    while (true) {
        FD_ZERO(&readfds);
        FD_SET(sock_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        const int max_fd = std::max(sock_fd, STDIN_FILENO);

        // Таймаут, чтобы периодически будиться для Ping/Ack‑таймеров
        timeval t_v{};
        t_v.tv_sec = 0;
        t_v.tv_usec = SELECT_TIMEOUT_USEC;  // 500 мс

        const int ret = ::select(max_fd + 1, &readfds, nullptr, nullptr, &t_v);

        if (ret < 0) {
            if (errno == EINTR) {
                if (shutdown_requested.load(std::memory_order_relaxed)) {
                    FD_ZERO(&readfds);
                    return;  // EINTR по Ctrl-C - выход и далее завершение
                             // приложения
                }
                continue;  // повторить select()
            }

            if (errno == EAGAIN) {  // некретичная ошибка
                FD_ZERO(&readfds);
                return;  // пробудиться без событий
            }
            // реальная ошибка select()
            messenger::utils::throw_system_error("select");
        }

        // выход с ret == 0 (таймаут) или > 0 (по событию)
        return;
    }
}

bool handle_peer(int socket_fd) {
    messenger::proto::Message msg{};
    bool disconnected = false;

    const bool okey =
        messenger::proto::receive_msg(socket_fd, msg, disconnected);

    if (!okey) {
        clearInputLine();
        std::cout << "\nФатальная ошибка протокола: повреждённый пакет\n";
        redrawInput();
        // в дальнейшем логирование и/или логика обработки
        return false;
    }

    if (disconnected) {
        std::cout << "\nСобеседник отключился.\n";
        return false;
    }

    // Обработка Ack: проверка на ожидаемый id
    if (msg.type == messenger::proto::MsgType::Ack) {
        auto ack_it = pending_acks.find(msg.id);
        if (ack_it != pending_acks.end()) {
            clearInputLine();
            std::cout << "\n[Сообщение msg_id=" << msg.id << " доставлено]\n";
            redrawInput();

            for (auto& outgoing_message : undelivered_messages) {
                if (outgoing_message.message_id == msg.id) {
                    outgoing_message.delivered = true;
                    break;
                }
            }

            pending_acks.erase(ack_it);
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
    ssize_t bytes_read{0};
    while (true) {
        bytes_read = ::read(STDIN_FILENO, &key, 1);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                if (shutdown_requested.load(std::memory_order_relaxed)) {
                    return false;
                }
                continue;
            }
            if (errno == EAGAIN) {
                continue;
            }
            std::cout << "\n[Ошибка чтения stdin]\n";
            return true;
        }
        break;
    }

    if (bytes_read == 0) {
        std::cout << "\n[Системный EOF на stdin]\n";
        return false;
    }

    // ====== ВЫХОД ПО ОСОБЫМ КЛАВИШАМ ======
    // Ctrl-D (EOF)
    if (key == '\x04') {
        std::cout << "\nВыход по Ctrl-D\n";
        return false;
    }

    // Ctrl-C — обрабатывается через обработчик сигнала SIGINT
    // (handleExitSignal), поэтому здесь символ '\x03' мы не ловим. В raw-mode
    // он не доходит как символ, а сразу превращается в сигнал.

    // ====== ENTER ======
    if (key == '\n' || key == '\r') {
        // Команда выхода /выход или /exit
        if (input_buffer == "/выход" || input_buffer == "/exit") {
            std::cout << "\nОтключаемся...\n";
            return false;
        }

        if (input_buffer.starts_with("/повтор")) {
            clearInputLine();
            handleRepeatCommand(socket_fd, input_buffer);
            input_buffer.clear();
            typing_sent = false;
            redrawInput();
            return true;
        }

        // Команда показать историю сообщений
        if (input_buffer == "/история") {
            clearInputLine();
            showHistory();
            input_buffer.clear();
            typing_sent = false;
            redrawInput();
            return true;
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

                const std::string history_line = "[Я]: " + input_buffer;
                chat_history.push_back(history_line);
                appendToHistoryFile(history_line);
                if (chat_history.size() > MAX_HISTORY_LINES) {
                    chat_history.erase(chat_history.begin());
                }

                // Запуск неблокирующего ожидания Ack: запомнить, что ждём его
                PendingAck ack_state{};
                ack_state.id = msg_id;
                ack_state.deadline =
                    Clock::now() + std::chrono::seconds(ACK_TIMEOUT_SECONDS);
                ack_state.retry_count = 0;
                ack_state.last_payload = input_buffer;
                pending_acks[msg_id] = ack_state;

                OutgoingMessage outgoing_message{};
                outgoing_message.message_id = msg_id;
                outgoing_message.payload = input_buffer;
                outgoing_message.delivered = false;
                undelivered_messages.push_back(outgoing_message);
                if (undelivered_messages.size() > MAX_UNDELIVERED_MESSAGES) {
                    undelivered_messages.erase(undelivered_messages.begin());
                }
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
            eraseLastUtf8Char(input_buffer);
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

    // Установить обработчики сигналов
    {
        struct sigaction sig_action {};
        sig_action.sa_handler = handleExitSignal;  // новый обработчик
        sigemptyset(&sig_action.sa_mask);
        sig_action.sa_flags = SA_RESTART;  // автоматически перезапускать
                                           // системные вызовы после сигнала

        sigaction(SIGINT, &sig_action, nullptr);   // Ctrl-C
        sigaction(SIGTERM, &sig_action, nullptr);  // kill
        sigaction(SIGSEGV, &sig_action, nullptr);  // segmentation fault
        sigaction(SIGABRT, &sig_action, nullptr);  // abort()

        // Обработчик SIGPIPE, чтобы send() не убивал процесс при записи в
        // закрытый сокет
        struct sigaction sig_action_ign {};
        sig_action_ign.sa_handler = SIG_IGN;  // Игнорировать SIGPIPE
        sigemptyset(&sig_action_ign.sa_mask);
        sig_action_ign.sa_flags = 0;
        sigaction(SIGPIPE, &sig_action_ign, nullptr);
    }

    const TerminalRawGuard term_guard;

    loadHistoryFromFile();

    std::cout << "Чат готов. Печатай сообщение и жми Enter.\n"
              << "Команда выхода: /выход или /exit, а также Ctrl-D.\n\n";
    redrawInput();

    while (!shutdown_requested.load(std::memory_order_relaxed)) {
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
