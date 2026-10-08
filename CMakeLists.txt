cmake_minimum_required(VERSION 3.15)

project(CodingCLI
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
# FileManager test executable
# --------------------------------------------------

add_executable(FileManagerTest
    tests/FileManagerTest.cpp
    src/FileManager.cpp
)

target_include_directories(FileManagerTest
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

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