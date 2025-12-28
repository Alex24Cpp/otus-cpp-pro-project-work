#include <gtest/gtest.h>

#include <system_error>
#include <cerrno>

#include "p2p_error.h"


// ============= Тест для функции throw_system_error =============
TEST(ThrowSystemErrorTest, ThrowsSystemErrorWithCorrectMessageAndErrno) {
    errno = EINVAL; // задать errno для теста
try {
    throw_system_error("test message");
    FAIL() << "Expected std::system_error to be thrown";
}
catch (const std::system_error& e) {
    // Проверка кода ошибки
    EXPECT_EQ(e.code().value(), EINVAL);
    // Проверка категории ошибки
    EXPECT_EQ(&e.code().category(), &std::system_category());
    // Проверка сообщения
    std::string msg = e.what();
    EXPECT_NE(msg.find("test message"), std::string::npos)
        << "Error message should contain the provided text";
    }
    catch (...) {
        FAIL() << "Expected std::system_error, but caught a different exception";
    }
}

