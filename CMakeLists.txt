cmake_minimum_required(VERSION 3.15)

project(CodingCLI
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(codeagent
    src/main.cpp
    src/CLIController.cpp
)

target_include_directories(codeagent
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(codeagent PRIVATE /W4)
else()
    target_compile_options(codeagent PRIVATE -Wall -Wextra -Wpedantic)
endif()