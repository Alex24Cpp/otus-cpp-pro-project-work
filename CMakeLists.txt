cmake_minimum_required(VERSION 3.16)

set(PROJECT_VERSION "0.0.1" CACHE INTERNAL "Project version")

project(messenger VERSION ${PROJECT_VERSION})

find_package(GTest REQUIRED)

# Включает генерацию compile_commands.json (нужно для clang-tidy)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# clang-tidy
option(CLANG-TIDY "Should do checking or not" OFF)
message(STATUS "<<CLANG-TIDY: ${CLANG-TIDY}>>")
if (CLANG-TIDY)
    set(CMAKE_CXX_CLANG_TIDY "clang-tidy;-p=${CMAKE_BINARY_DIR}")
endif()

add_executable(messenger
    src/messenger.cpp

    src/app/p2p_chat.cpp
    src/app/p2p_chat.h

    src/utils/p2p_error.cpp
    src/utils/p2p_error.h

    src/net/raii_socket.cpp
    src/net/raii_socket.h
    src/net/client_socket.cpp
    src/net/client_socket.h
    src/net/server_socket.cpp
    src/net/server_socket.h
    src/net/net_api.cpp
    src/net/net_api.h

    src/protocol/message.hpp
    src/protocol/protocol_api.cpp
    src/protocol/protocol_api.h
    src/protocol/serializer.cpp
    src/protocol/serializer.h
)
add_executable(gtest_messenger
    test/gtest_messenger.cpp
    src/utils/p2p_error.cpp
    src/utils/p2p_error.h
    src/net/raii_socket.cpp
    src/net/raii_socket.h
    src/net/client_socket.cpp
    src/net/client_socket.h
    src/net/server_socket.cpp
    src/net/server_socket.h
    # src/app/p2p_chat.cpp
    # src/app/p2p_chat.h
)

set_target_properties(
    messenger 
    gtest_messenger 
    PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(messenger
    PRIVATE src
)

target_include_directories(gtest_messenger
    PRIVATE ${GTEST_INCLUDE_DIRS} "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(gtest_messenger
    ${GTEST_BOTH_LIBRARIES}
)

# clang-format
option(CLANG-FORMAT "Should do formatting or not" OFF)
message(STATUS "<<CLANG-FORMAT: ${CLANG-FORMAT}>>")
if (CLANG-FORMAT)
    find_program(CLANG-FORMAT_PATH NAMES clang-format REQUIRED)
    message(STATUS "Найден clang-format: ${CLANG-FORMAT_PATH}")
    file(GLOB_RECURSE CLANG-FORMAT_SRC CONFIGURE_DEPENDS *.*pp)
    # исключить все пути, содержащие /CMakeFiles/ (или \CMakeFiles\)
    # и все пути, содержащие /googletest/ (или \googletest\)
    list(FILTER CLANG-FORMAT_SRC EXCLUDE REGEX ".*/CMakeFiles/.*|.*/googletest/.*")
    message(STATUS "Найдены src: ${CLANG-FORMAT_SRC}")
    add_custom_target(clangformat
        COMMAND ${CLANG-FORMAT_PATH} -i ${CLANG-FORMAT_SRC}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Форматирование ${PROJECT_NAME} с ${CLANG-FORMAT_PATH}"
    )
    add_dependencies(messenger clangformat)
    add_dependencies(gtest_messenger clangformat)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "<<Сборка: ${CMAKE_BUILD_TYPE}>>")
    target_compile_options(messenger PRIVATE
        -Wall
        -Wextra
        -pedantic
        -Werror
        -Wconversion
        -Wsign-conversion
        # no parameter
        # -Wno-unused-parameter
        # only for debug and test builds!
        # -fsanitize=address
        # -fsanitize=undefined
        # -fsanitize=leak
        # -fsanitize=thread
    )
    target_link_options(messenger PRIVATE 
        # -fsanitize=address
        # -fsanitize=undefined
        # -fsanitize=leak
        # -fsanitize=thread
    )
endif ()

set(CMAKE_INSTALL_PREFIX ".") # установка в текущую директорию проекта (для локального тестирования)
message(STATUS "<<CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}>>")

install(TARGETS messenger RUNTIME DESTINATION bin)

# тестовый бинарник
install(TARGETS gtest_messenger RUNTIME DESTINATION tests)

set(CPACK_GENERATOR DEB)

# основной пакет
set(CPACK_DEBIAN_PACKAGE_NAME "messenger")
set(CPACK_DEBIAN_PACKAGE_COMPONENT "runtime")

# пакет с тестами
set(CPACK_DEBIAN_TESTS_PACKAGE_NAME "gtest_messenger")
set(CPACK_DEBIAN_TESTS_PACKAGE_COMPONENT "tests")

set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")

set(CPACK_PACKAGE_CONTACT mr.tutu2014@mail.ru)

include(CPack)

enable_testing()
add_test(gtest_messenger gtest_messenger) 
