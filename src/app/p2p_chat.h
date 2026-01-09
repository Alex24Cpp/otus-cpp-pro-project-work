#pragma once

#include <sys/select.h>

#include "net/raii_socket.h"

namespace messenger::app {

void wait_for_events(int sock_fd, fd_set& readfds);
bool handle_peer(int socket_fd);
bool handle_user(int socket_fd);
void chat_loop(messenger::net::Socket sock);

} // namespace messenger::app
