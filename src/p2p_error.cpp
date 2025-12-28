#include "p2p_error.h"

#include <system_error>
#include <cerrno>

[[noreturn]] void throw_system_error(std::string_view error_msg) {
    throw std::system_error( errno, std::system_category(), std::string(error_msg) );
}
