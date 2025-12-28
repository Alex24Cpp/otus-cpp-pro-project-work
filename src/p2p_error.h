#pragma once
#include <string_view>

[[noreturn]] void throw_system_error(std::string_view error_msg);