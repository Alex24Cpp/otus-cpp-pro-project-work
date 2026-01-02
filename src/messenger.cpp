#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "app/p2p_chat.h"
#include "net/client_socket.h"
#include "net/server_socket.h"

// ---------- main() ----------

int main(int argc, char* argv[]) {
    using namespace messenger;
    try {
        if (argc < 2) {
            std::cerr
                << "Использование:\n"
                << "  "
                << argv[0]  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                << " сервер <порт>\n"
                << "  "
                << argv[0]  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                << " клиент требуется <хост> <порт>\n";
            return EXIT_FAILURE;
        }

        const std::string_view mode{
            argv[1]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

        if (mode == "сервер") {
            if (argc != 3) {
                throw std::invalid_argument("сервер: требуется порт");
            }

            const uint16_t port = static_cast<uint16_t>(std::stoi(
                argv[2]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            auto sock = net::create_server_socket(port);
            app::chat_loop(std::move(sock));

        } else if (mode == "клиент") {
            if (argc != 4) {
                throw std::invalid_argument("клиент: требуется хост и порт");
            }

            const std::string_view host{
                argv[2]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const uint16_t port = static_cast<uint16_t>(std::stoi(
                argv[3]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

            auto sock = net::create_client_socket(host, port);
            app::chat_loop(std::move(sock));

        } else {
            throw std::invalid_argument("Неизвестный режим: " +
                                        std::string(mode));
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "Фатальная ошибка: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }
}
