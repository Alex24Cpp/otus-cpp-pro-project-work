#pragma once

#include "net/raii_socket.h"

#include <sys/select.h>
#include <vector>

namespace messenger::app {

void wait_for_events(int sock_fd, fd_set& readfds);
bool handle_peer(int sock_fd, std::vector<char>& buffer);
bool handle_user(int sock_fd);
void chat_loop(messenger::net::Socket sock);

} // namespace messenger::app
