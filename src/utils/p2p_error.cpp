#include "utils/p2p_error.h"

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

namespace messenger::utils {

[[noreturn]] void throw_system_error(std::string_view error_msg) {
    throw std::system_error(errno, std::system_category(),
                            std::string(error_msg));
}

}  // namespace messenger::utils
