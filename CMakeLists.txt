cmake_minimum_required(VERSION 3.24)

project(bank
    VERSION 0.1.0
    DESCRIPTION "Concurrency-focused bank management system backend"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(BANK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(BANK_BUILD_TESTS "Build the unit test suite" ON)

include(GNUInstallDirs)

#
# Warning discipline. These are our own compile options so they never leak
# into third-party headers (find_package include dirs are SYSTEM already).
#
function(bank_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wconversion
            -Wdouble-promotion
        )
    endif()
    if(BANK_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

#
# Dependencies
#
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

#
# Core library
#
add_library(bank
    src/concurrency/AccountLockManager.cpp
    src/concurrency/ThreadPool.cpp
    src/concurrency/TransactionProcessor.cpp
    src/concurrency/TransactionQueue.cpp
    src/domain/PasswordHasher.cpp
    src/domain/Transaction.cpp
    src/domain/TransactionOps.cpp
    src/persistence/Json.cpp
    src/persistence/Serializer.cpp
    src/persistence/SqliteDatabase.cpp
    src/persistence/SqliteHelpers.cpp
    src/persistence/SqliteAccountRepository.cpp
    src/persistence/SqliteCustomerRepository.cpp
    src/persistence/SqliteTransactionRepository.cpp
    src/persistence/SqliteAuditRepository.cpp
    src/persistence/WalLogger.cpp
    src/persistence/SnapshotStore.cpp
)
add_library(bank::bank ALIAS bank)

target_include_directories(bank PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# SQLite and OpenSSL stay internal to the library; callers talk to the domain
# and repository interfaces, not to the storage engine.
target_link_libraries(bank
    PRIVATE OpenSSL::Crypto SQLite::SQLite3
    PUBLIC  Threads::Threads
)

bank_enable_warnings(bank)

#
# GoogleTest / GoogleMock (vendored via FetchContent)
#
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

#
# Unit tests
#
if(BANK_BUILD_TESTS)
    enable_testing()

    add_executable(bank_tests
        tests/test_money.cpp
        tests/test_password.cpp
        tests/test_domain.cpp
        tests/test_json.cpp
        tests/test_codec.cpp
        tests/test_repositories.cpp
        tests/test_wal.cpp
        tests/test_account_locks.cpp
        tests/test_transaction_queue.cpp
        tests/test_thread_pool.cpp
        tests/test_transaction_processor.cpp
        tests/test_stress.cpp
    )
    target_link_libraries(bank_tests PRIVATE bank GTest::gtest_main)
    target_include_directories(bank_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    bank_enable_warnings(bank_tests)

    add_test(NAME bank_tests COMMAND bank_tests)

    add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        VERBATIM USES_TERMINAL
        DEPENDS bank_tests)
endif()