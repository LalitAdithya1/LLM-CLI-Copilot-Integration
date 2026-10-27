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

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

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
    src/UnifiedDiff.cpp
)

target_include_directories(codeagent
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(codeagent
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
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
    src/UnifiedDiff.cpp
)

target_include_directories(UnifiedDiffTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(UnifiedDiffTest
    PRIVATE
        GTest::gtest_main
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
    src/UnifiedDiff.cpp
)

target_include_directories(CLIControllerTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(CLIControllerTest
    PRIVATE
        GTest::gtest_main
        CURL::libcurl
        nlohmann_json::nlohmann_json
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
gtest_discover_tests(CLIControllerTest)

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
endif()
