#pragma once
#include <string_view>

namespace messenger::utils {

[[noreturn]] void throw_system_error(std::string_view error_msg);

} // namespace messenger::utils