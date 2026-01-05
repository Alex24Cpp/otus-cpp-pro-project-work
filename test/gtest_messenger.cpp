#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/p2p_chat.h"
#include "net/client_socket.h"
#include "net/raii_socket.h"
#include "net/server_socket.h"
#include "utils/p2p_error.h"

using namespace messenger;

// ============= Тест для функции throw_system_error =============
TEST(ThrowSystemErrorTest, ThrowsSystemErrorWithCorrectMessageAndErrno) {
    errno = EINVAL;  // задать errno для теста
    try {
        utils::throw_system_error("test message");
        FAIL() << "Expected std::system_error to be thrown";
    } catch (const std::system_error& e) {
        // Проверка кода ошибки
        EXPECT_EQ(e.code().value(), EINVAL);
        // Проверка категории ошибки
        EXPECT_EQ(&e.code().category(), &std::system_category());
        // Проверка сообщения
        const std::string msg = e.what();
        EXPECT_NE(msg.find("test message"), std::string::npos)
            << "Error message should contain the provided text";
    } catch (...) {
        FAIL()
            << "Expected std::system_error, but caught a different exception";
    }
}

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

using namespace messenger::net;

// ============= Тесты для функции create_client_socket =============

// ------------- Фикстура: минимальный тестовый сервер -------------
class CreateClientSockeTest : public ::testing::Test {
protected:
    uint16_t port = 55555;
    std::atomic<bool> ready = false;
    std::thread server_thread;

    void SetUp() override {
        ready = false;

        server_thread = std::thread([this] {
            const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(server_fd, 0) << "Не удалось создать серверный сокет";

            int opt = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            ASSERT_EQ(::bind(server_fd, (sockaddr*)&addr, sizeof(addr)), 0)
                << "bind() не удался";

            ASSERT_EQ(::listen(server_fd, 1), 0) << "listen() не удался";

            // Таймаут на accept()
            struct timeval time_v {};
            time_v.tv_sec = 1;  // 1 секунда таймаута
            time_v.tv_usec = 0;
            setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &time_v,
                       sizeof(time_v));

            // Сервер готов принимать соединения
            ready.store(true);

            // Сервер принимает одно соединение и завершается
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            const int client_fd = ::accept(server_fd, (sockaddr*)&client, &len);

            if (client_fd >= 0) {
                ::close(client_fd);
            }

            ::close(server_fd);
        });

        // Ожидание пока сервер поднимется
        while (!ready.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void TearDown() override {
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
};

// Успешное подключение
TEST_F(CreateClientSockeTest, ConnectsSuccessfully) {
    EXPECT_NO_THROW({ auto client = create_client_socket("127.0.0.1", port); });
}

// Проверка если неверный IP, то - std::runtime_error
TEST_F(CreateClientSockeTest, ThrowsOnInvalidHost) {
    EXPECT_THROW(create_client_socket("256.256.256.256", port),
                 std::runtime_error);
}

// Проверка, что функция возвращает объект типа Socket
TEST_F(CreateClientSockeTest, ReturnsSocketObject) {
    auto client = create_client_socket("127.0.0.1", port);
    static_assert(std::is_same_v<decltype(client), Socket>);
}

// Проверка, что сокет действительно открыт
TEST_F(CreateClientSockeTest, ReturnedSocketIsValid) {
    const Socket client = create_client_socket("127.0.0.1", port);

    EXPECT_GE(client.fd_return(), 0);
}

// Интеграционный тест: сокет действительно работает
TEST_F(CreateClientSockeTest, CanSendDataAfterConnect) {
    auto client = create_client_socket("127.0.0.1", port);

    const char msg = 'X';
    EXPECT_EQ(::send(client.fd_return(), &msg, 1, 0), 1);
}

// ============= Тест для create_client_socket без сервера =============
// Проверка если системная ошибка подключения (порт закрыт), то -
// utils::p2p_error
TEST(ClientSocketStandaloneTest, ThrowsOnConnectionRefused) {
    const uint16_t closed_port = 59999;

    EXPECT_THROW(create_client_socket("127.0.0.1", closed_port),
                 std::system_error);
}

// ============= Тесты класса Socket =============
// Конструктор сохраняет fd
TEST(SocketClassTest, StoresFileDescriptor) {
    const Socket socket(5);
    EXPECT_EQ(socket.fd_return(), 5);
}

// Move‑конструктор корректно переносит владение
TEST(SocketClassTest, MoveConstructorTransfersOwnership) {
    Socket socket1(10);
    const Socket socket2(std::move(socket1));

    EXPECT_EQ(socket2.fd_return(), 10);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move, bugprone-use-after-move)
    EXPECT_EQ(socket1.fd_return(), -1);
}

// Move‑оператор корректно переносит владение
TEST(SocketClassTest, MoveAssignmentTransfersOwnership) {
    Socket socket1(11);
    Socket socket2(22);

    socket2 = std::move(socket1);

    EXPECT_EQ(socket2.fd_return(), 11);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move, bugprone-use-after-move)
    EXPECT_EQ(socket1.fd_return(), -1);
}

// Деструктор закрывает сокет
TEST(SocketClassTest, DestructorClosesSocket) {
    const int fd_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    { const Socket socket(fd_socket); }
    // Попытка повторно закрыть должна дать EBADF
    EXPECT_EQ(::close(fd_socket), -1);
    EXPECT_EQ(errno, EBADF);
}

// ============= Тесты для функции create_server_socket =============

// ------------- Фикстура -------------
class CreateServerSocketTest : public ::testing::Test {
protected:
    uint16_t port = 55555;
};

// Тест что создается сокет и принимается подключение
TEST_F(CreateServerSocketTest, AcceptsClientConnection) {
    std::atomic<bool> server_ready = false;

    // Сервер в отдельном потоке
    std::thread server([&] {
        server_ready.store(true);
        const Socket client = create_server_socket(port);
        EXPECT_GE(client.fd_return(), 0);
    });

    // Ожидание, пока сервер начнёт listen()
    while (!server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Клиент подключается
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ASSERT_EQ(::connect(sock, (sockaddr*)&addr, sizeof(addr)), 0);

    ::close(sock);
    server.join();
}

// Проверка, что сервер возвращает объект типа Socket
TEST_F(CreateServerSocketTest, ReturnsSocketObject) {
    std::atomic<bool> ready = false;

    std::thread server([&] {
        ready.store(true);
        auto client = create_server_socket(port);
        static_assert(std::is_same_v<decltype(client), Socket>);
    });

    while (!ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ASSERT_EQ(::connect(sock, (sockaddr*)&addr, sizeof(addr)), 0);

    ::close(sock);
    server.join();
}

// Проверка, что выбрасывается исключение при занятом порте
TEST_F(CreateServerSocketTest, ThrowsIfPortBusy) {
    // Открываем сокет и занимаем порт
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);
    int opt = 1;
    ASSERT_EQ(::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
              0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ASSERT_EQ(::bind(sock, (sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(sock, 1), 0);

    // Проверка, что create_server_socket падает на bind()
    EXPECT_THROW(create_server_socket(port), std::system_error);

    ::close(sock);
}

// Проверка, что принимаются данные от собеседника
TEST_F(CreateServerSocketTest, ServerReceivesData) {
    std::atomic<bool> ready = false;

    std::thread server([&] {
        ready.store(true);
        const Socket client = create_server_socket(port);

        char buf{};
        const ssize_t num = ::recv(client.fd_return(), &buf, 1, 0);
        EXPECT_EQ(num, 1);
        EXPECT_EQ(buf, 'X');
    });

    while (!ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ASSERT_EQ(::connect(sock, (sockaddr*)&addr, sizeof(addr)), 0);

    char msg = 'X';
    ASSERT_EQ(::send(sock, &msg, 1, 0), 1);

    ::close(sock);
    server.join();
}

// ============= Тесты для функций Основной цикл чата =============
using namespace messenger::app;

// ------------- Фикстура: минимальный тестовый сервер -------------
class P2PChatTest : public ::testing::Test {
protected:
    int sock_user{};
    int sock_peer{};

    void SetUp() override {
        std::array<int, 2> sock_p{};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sock_p.data()), 0);
        sock_user = sock_p[0];
        sock_peer = sock_p[1];
    }

    void TearDown() override {
        ::close(sock_user);
        ::close(sock_peer);
    }
};

// Тест для wait_for_events
TEST_F(P2PChatTest, WaitForEventsDetectsPeerData) {
    fd_set readfds;

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const char msg = 'A';
        ::send(sock_peer, &msg, 1, 0);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    wait_for_events(sock_user, readfds);

    EXPECT_TRUE(FD_ISSET(sock_user, &readfds));

    writer.join();
}

// Тест для handle_peer — получение данных
TEST_F(P2PChatTest, HandlePeerReceivesMessage) {
    std::vector<char> buffer(4096);

    const std::string msg = "Hello";
    ASSERT_EQ(::send(sock_peer, msg.data(), msg.size(), 0), msg.size());

    const bool result = handle_peer(sock_user, buffer);

    EXPECT_TRUE(result);
    EXPECT_EQ(std::string(buffer.data(), 5), "Hello");
}

// Тест для handle_peer — собеседник отключился
TEST_F(P2PChatTest, HandlePeerDetectsDisconnect) {
    std::vector<char> buffer(4096);

    ::close(sock_peer);  // имитируем отключение

    const bool result = handle_peer(sock_user, buffer);

    EXPECT_FALSE(result);
}

// Тест для handle_user — отправка данных
TEST_F(P2PChatTest, HandleUserSendsMessage) {
    // Подмена stdin
    const std::istringstream fake_input("Hello\n");
    std::streambuf* old = std::cin.rdbuf(fake_input.rdbuf());

    const bool result = handle_user(sock_user);

    EXPECT_TRUE(result);

    std::array<char, 16> buf{};
    const ssize_t bytes_received =
        ::recv(sock_peer, buf.data(), sizeof(buf), 0);

    EXPECT_EQ(bytes_received, 5);
    EXPECT_EQ(std::string(buf.data(), 5), "Hello");

    std::cin.rdbuf(old);
}

// Тест для handle_user — команда выхода
TEST_F(P2PChatTest, HandleUserExitCommand) {
    const std::istringstream fake_input("/выход\n");
    std::streambuf* old = std::cin.rdbuf(fake_input.rdbuf());

    const bool result = handle_user(sock_user);

    EXPECT_FALSE(result);

    std::cin.rdbuf(old);
}

// Тест для handle_user — EOF на stdin
TEST_F(P2PChatTest, HandleUserEOF) {
    const std::istringstream fake_input("");
    std::streambuf* old = std::cin.rdbuf(fake_input.rdbuf());

    const bool result = handle_user(sock_user);

    EXPECT_FALSE(result);

    std::cin.rdbuf(old);
}

// Тест для chat_loop — выход при отключении собеседника
TEST_F(P2PChatTest, ChatLoopStopsOnPeerDisconnect) {
    messenger::net::Socket sock(sock_user);

    std::thread thr([&] { chat_loop(std::move(sock)); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ::close(sock_peer);  // отключаем собеседника

    thr.join();
}

// Тест для chat_loop — выход по команде пользователя
TEST_F(P2PChatTest, ChatLoopStopsOnUserExit) {
    Socket sock(sock_user);

    // Создаём pipe
    std::array<int, 2> pipefd{};
    ASSERT_EQ(::pipe(pipefd.data()), 0);

    // Сохраняем оригинальный stdin
    const int old_stdin = ::dup(STDIN_FILENO);
    ASSERT_GE(old_stdin, 0);

    // Подменяем stdin на pipefd[0]
    ASSERT_EQ(::dup2(pipefd[0], STDIN_FILENO), STDIN_FILENO);

    // Пишем команду выхода в pipe
    const std::string exit_cmd = "/exit\n";
    ASSERT_EQ(::write(pipefd[1], exit_cmd.data(), exit_cmd.size()),
              static_cast<ssize_t>(exit_cmd.size()));

    ::close(pipefd[1]);  // Закрываем запись → chat_loop увидит EOF

    // Запускаем chat_loop
    std::thread thr([&] { chat_loop(std::move(sock)); });

    thr.join();

    // Возвращаем stdin обратно
    ::dup2(old_stdin, STDIN_FILENO);
    ::close(old_stdin);
    ::close(pipefd[0]);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)