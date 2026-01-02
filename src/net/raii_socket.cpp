#include "net/raii_socket.h"

#include <unistd.h>

#include <stdexcept>

namespace messenger::net {

// RAII-обёртка для сокета
Socket::Socket(int sock_fd) : fd_(sock_fd) {
    if (fd_ < 0) {
        throw std::invalid_argument("Socket: недопустимый файловый дескриптор");
    }
}

Socket::~Socket() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

int Socket::fd_return() const {
    return fd_;
}

}  // namespace messenger::net