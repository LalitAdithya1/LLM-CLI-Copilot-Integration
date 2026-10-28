cmake_minimum_required(VERSION 3.15)

# Silence the FetchContent timestamp policy warning
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(CodingCLI
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------
# Find required libraries
# --------------------------------------------------
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# --------------------------------------------------
# Fetch GoogleTest automatically
# --------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.58.0.tar.gz
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# This loopback server does not use TLS or compression.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

enable_testing()

# --------------------------------------------------
# GUI HTTP server component
# --------------------------------------------------
# Exception hierarchy shared by the CLI, the LLM client, and the GUI server.
add_library(CodeAgentError
    src/CodeAgentError.cpp
)

target_include_directories(CodeAgentError
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_library(UnifiedDiff
    src/UnifiedDiff.cpp
)

target_include_directories(UnifiedDiff
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_library(GuiHttpServer
    src/GuiHttpServer.cpp
    src/NativeDialog.cpp
)

target_include_directories(GuiHttpServer
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(GuiHttpServer
    PUBLIC
        httplib::httplib
        nlohmann_json::nlohmann_json
        UnifiedDiff
        CodeAgentError
)

# --------------------------------------------------
# Main application
# --------------------------------------------------
add_executable(codeagent
    src/main.cpp
    src/CLIController.cpp
    src/FileManager.cpp
    src/PromptBuilder.cpp
    src/ResponseParser.cpp
    src/LLMClient.cpp
    src/EnvLoader.cpp
)

target_include_directories(codeagent
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(codeagent
    PRIVATE
        GuiHttpServer
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

# Last-resort frontend location for `codeagent gui` when the executable is not
# next to (or installed with) the frontend directory.
target_compile_definitions(codeagent
    PRIVATE
        CODEAGENT_FRONTEND_SOURCE_DIR="${PROJECT_SOURCE_DIR}/frontend"
)

install(TARGETS codeagent RUNTIME DESTINATION bin)
install(DIRECTORY frontend/
    DESTINATION share/codeagent/frontend
    PATTERN "tests" EXCLUDE
)

# --------------------------------------------------
# FileManager tests
# --------------------------------------------------
add_executable(FileManagerTest
    tests/FileManagerTest.cpp
    src/FileManager.cpp
)

target_include_directories(FileManagerTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(FileManagerTest
    PRIVATE
        GTest::gtest_main
        CodeAgentError
)

# --------------------------------------------------
# PromptBuilder tests
# --------------------------------------------------
add_executable(PromptBuilderTest
    tests/PromptBuilderTest.cpp
    src/PromptBuilder.cpp
)

target_include_directories(PromptBuilderTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(PromptBuilderTest
    PRIVATE
        GTest::gtest_main
)

# --------------------------------------------------
# LLMClient tests
# --------------------------------------------------
add_executable(LLMClientTest
    tests/LLMClientTest.cpp
    src/LLMClient.cpp
    src/ResponseParser.cpp
    src/EnvLoader.cpp
)

target_include_directories(LLMClientTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(LLMClientTest
    PRIVATE
        GTest::gtest_main
        CodeAgentError
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

# --------------------------------------------------
# ResponseParser tests
# --------------------------------------------------
add_executable(ResponseParserTest
    tests/ResponseParserTest.cpp
    src/ResponseParser.cpp
)

# --------------------------------------------------
# GUI HTTP server tests
# --------------------------------------------------
add_executable(GuiHttpServerTest
    tests/GuiHttpServerTest.cpp
)

target_link_libraries(GuiHttpServerTest
    PRIVATE
        GTest::gtest_main
        GuiHttpServer
)

target_include_directories(ResponseParserTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(ResponseParserTest
    PRIVATE
        GTest::gtest_main
        nlohmann_json::nlohmann_json
)

# --------------------------------------------------
# Unified diff tests
# --------------------------------------------------
add_executable(UnifiedDiffTest
    tests/UnifiedDiffTest.cpp
)

target_include_directories(UnifiedDiffTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(UnifiedDiffTest
    PRIVATE
        GTest::gtest_main
        UnifiedDiff
)

# --------------------------------------------------
# .env loading tests
# --------------------------------------------------
add_executable(EnvLoaderTest
    tests/EnvLoaderTest.cpp
    src/EnvLoader.cpp
)

target_include_directories(EnvLoaderTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(EnvLoaderTest
    PRIVATE
        GTest::gtest_main
        CodeAgentError
)

# --------------------------------------------------
# CLI refactoring workflow tests
# --------------------------------------------------
add_executable(CLIControllerTest
    tests/CLIControllerTest.cpp
    src/CLIController.cpp
    src/FileManager.cpp
    src/PromptBuilder.cpp
    src/LLMClient.cpp
    src/ResponseParser.cpp
    src/EnvLoader.cpp
)

target_include_directories(CLIControllerTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(CLIControllerTest
    PRIVATE
        GTest::gtest_main
        GuiHttpServer
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

# --------------------------------------------------
# Exception hierarchy tests
# --------------------------------------------------
add_executable(CodeAgentErrorTest
    tests/CodeAgentErrorTest.cpp
)

target_link_libraries(CodeAgentErrorTest
    PRIVATE
        GTest::gtest_main
        CodeAgentError
)

# --------------------------------------------------
# Test discovery
# --------------------------------------------------
include(GoogleTest)

gtest_discover_tests(FileManagerTest)
gtest_discover_tests(PromptBuilderTest)
gtest_discover_tests(LLMClientTest)
gtest_discover_tests(ResponseParserTest)
gtest_discover_tests(UnifiedDiffTest)
gtest_discover_tests(EnvLoaderTest)
gtest_discover_tests(CLIControllerTest)
gtest_discover_tests(GuiHttpServerTest)
gtest_discover_tests(CodeAgentErrorTest)

# --------------------------------------------------
# Compiler warnings
# --------------------------------------------------
if(MSVC)
    target_compile_options(codeagent PRIVATE /W4)
    target_compile_options(FileManagerTest PRIVATE /W4)
    target_compile_options(PromptBuilderTest PRIVATE /W4)
    target_compile_options(LLMClientTest PRIVATE /W4)
    target_compile_options(ResponseParserTest PRIVATE /W4)
    target_compile_options(UnifiedDiffTest PRIVATE /W4)
    target_compile_options(CLIControllerTest PRIVATE /W4)
    target_compile_options(GuiHttpServer PRIVATE /W4)
    target_compile_options(UnifiedDiff PRIVATE /W4)
    target_compile_options(GuiHttpServerTest PRIVATE /W4)
else()
    target_compile_options(codeagent PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(FileManagerTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(PromptBuilderTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(LLMClientTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(ResponseParserTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(UnifiedDiffTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(CLIControllerTest PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(GuiHttpServer PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(UnifiedDiff PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_compile_options(GuiHttpServerTest PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()
