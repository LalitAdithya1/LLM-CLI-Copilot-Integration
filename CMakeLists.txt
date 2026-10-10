cmake_minimum_required(VERSION 3.15)

# Silence the FetchContent timestamp policy warning
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(CodingCLI
    VERSION 1.0
    LANGUAGES CXX
)

project(CodingCLI
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------
# Fetch GoogleTest automatically
# --------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# Prevent GTest from overriding parent compiler settings on MSVC
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
)

target_include_directories(codeagent
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

# --------------------------------------------------
# Google Test Executable
# --------------------------------------------------
add_executable(FileManagerTest
    tests/FileManagerTest.cpp
    src/FileManager.cpp
)

target_include_directories(FileManagerTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

# Link against gtest_main (automatically supplies main() for the tests)
target_link_libraries(FileManagerTest
    PRIVATE
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(FileManagerTest)

# --------------------------------------------------
# Compiler warnings
# --------------------------------------------------
if(MSVC)
    target_compile_options(codeagent PRIVATE /W4)
    target_compile_options(FileManagerTest PRIVATE /W4)
else()
    target_compile_options(codeagent PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    target_compile_options(FileManagerTest PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()