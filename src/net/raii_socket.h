#pragma once
#include <unistd.h>

namespace messenger::net {

// RAII-обёртка для сокета

class Socket {
public:
    explicit Socket(int sock_fd);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;

    Socket& operator=(Socket&& other) noexcept;

    int fd_return() const;

private:
    int fd_;
};
} // namespace messenger::net