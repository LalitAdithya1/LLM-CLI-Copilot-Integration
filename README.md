# AI Code Generation and Refactoring Assistant

A cross-platform, terminal-based C++ application that uses a Large Language Model (LLM) API to generate code, analyze existing source files, and suggest refactoring improvements.

The application provides a command-line interface (CLI) for interacting with an LLM, managing source files, and reviewing AI-generated code changes. It is designed to work across Windows, Linux, and macOS with a shared C++ codebase.

## 1. Project Overview

The AI Code Generation and Refactoring Assistant helps developers improve their productivity by integrating AI-powered code generation and refactoring into a terminal workflow.

Users can provide instructions to generate new code, analyze existing code for potential improvements, and review suggested changes before applying them to their projects.

The application communicates with an external LLM API. It does not train or host its own language model.

## 2. Objectives

* Develop a cross-platform terminal application using C++.
* Integrate an external LLM API for code generation and refactoring.
* Read and process local source-code files.
* Construct prompts using user instructions and source-code context.
* Parse API responses and present results clearly in the terminal.
* Support multiple programming languages as input.
* Protect original source files by requiring user approval before applying modifications.
* Maintain a portable codebase that can be compiled on Windows, Linux, and macOS.

## 3. Core Features

### Code Generation

* Accept natural-language instructions from the user.
* Generate code based on the provided requirements.
* Display generated code in the terminal.
* Optionally save generated code to a user-specified file.
* Support specifying the desired programming language.

### Code Refactoring

* Read source-code files from a specified project directory.
* Submit relevant code and refactoring instructions to the LLM.
* Suggest improvements in readability, maintainability, and structure.
* Display suggested changes and explanations.
* Present a diff between the original and proposed code where practical.
* Apply changes only after explicit user approval.

### File Management

* Read individual source files.
* Support processing selected files within a project directory.
* Save generated code and approved refactoring results.
* Preserve original files unless the user explicitly approves changes.
* Handle missing files, unsupported file types, and file-access errors.

### LLM Integration

* Send HTTP requests to an external LLM API.
* Construct prompts using source code and user instructions.
* Parse JSON responses.
* Handle network errors, API failures, rate limits, and malformed responses.
* Load API credentials from environment variables or local configuration that is excluded from version control.

### Terminal Interface

* Provide a command-based interface.
* Display generated code, refactoring suggestions, and error messages.
* Support commands such as `generate`, `refactor`, `help`, and `exit`.
* Validate user input and provide clear usage instructions.

## 4. Technology Stack

| Component              | Technology                               |
| ---------------------- | ---------------------------------------- |
| Programming language   | C++17 or later                           |
| Build system           | CMake                                    |
| HTTP client            | libcurl                                  |
| JSON parsing           | nlohmann/json                            |
| Filesystem operations  | C++ Standard Library (`std::filesystem`) |
| File input/output      | C++ Standard Library                     |
| AI integration         | External LLM API                         |
| Version control        | Git                                      |
| Cross-platform testing | GitHub Actions (optional)                |

Dependencies should be selected and configured to support the operating systems targeted by the project.

## 5. System Architecture

The application is organized into modular components.

1. **CLI Controller:** Parses commands and coordinates the workflow.
2. **File Manager:** Reads source files and saves generated or approved code.
3. **Prompt Builder:** Combines user instructions with source-code context.
4. **LLM Client:** Sends HTTP requests and receives responses from the LLM API.
5. **Response Parser:** Extracts generated code, explanations, and proposed changes.
6. **Code Generator:** Handles code-generation requests.
7. **Code Refactorer:** Handles source-code analysis and refactoring requests.
8. **Diff and Approval Manager:** Displays changes and obtains user approval before modifying files.

### Workflow

For code generation:

1. The user enters a generation command.
2. The application collects the user's requirements.
3. The prompt builder constructs the request.
4. The LLM client sends the request to the configured API.
5. The response parser extracts the result.
6. The application displays the generated code and optionally saves it.

For refactoring:

1. The user specifies a source file or project directory.
2. The file manager reads the selected source code.
3. The prompt builder creates the refactoring request.
4. The LLM client submits the request to the API.
5. The response parser extracts the proposed changes.
6. The application displays the suggestions and, where supported, a diff.
7. The user reviews and approves the changes before they are saved.

## 6. Proposed Project Structure

```text
AI-Code-Assistant/
├── include/
│   ├── CLIController.hpp
│   ├── FileManager.hpp
│   ├── PromptBuilder.hpp
│   ├── LLMClient.hpp
│   ├── ResponseParser.hpp
│   ├── CodeGenerator.hpp
│   ├── CodeRefactorer.hpp
│   └── DiffManager.hpp
├── src/
│   ├── main.cpp
│   ├── CLIController.cpp
│   ├── FileManager.cpp
│   ├── PromptBuilder.cpp
│   ├── LLMClient.cpp
│   ├── ResponseParser.cpp
│   ├── CodeGenerator.cpp
│   ├── CodeRefactorer.cpp
│   └── DiffManager.cpp
├── tests/
│   ├── test_file_manager.cpp
│   ├── test_prompt_builder.cpp
│   └── test_response_parser.cpp
├── .gitignore
├── CMakeLists.txt
├── README.md
└── LICENSE
```

This is a proposed structure; the final organization can be adjusted during implementation.

## 7. Example Usage

The following commands illustrate the intended interface. Exact syntax will be finalized during implementation.

### Generate code

```bash
ai-assistant generate "Implement binary search in C++"
```

### Refactor a source file

```bash
ai-assistant refactor ./src/main.cpp
```

### Refactor with specific instructions

```bash
ai-assistant refactor ./src/main.cpp \
  --instruction "Improve readability and reduce duplication"
```

### Generate code and save the result

```bash
ai-assistant generate "Implement a stack using arrays" \
  --language cpp \
  --output ./generated/stack.cpp
```

### Display help

```bash
ai-assistant help
```

These examples describe the planned interface, not necessarily commands available in the current implementation.

## 8. Requirements

* A C++17-compatible compiler.
* CMake.
* Git.
* libcurl.
* nlohmann/json.
* An API key for a compatible LLM provider.
* An internet connection for API requests.

## 9. Build and Installation

The project aims to support Windows, Linux, and macOS. The same C++ source code can be used across these systems, but the application must be compiled separately for the target operating system and architecture.

### Clone the repository

```bash
git clone <repository-url>
cd AI-Code-Assistant
```

### Configure and build

```bash
cmake -S . -B build
cmake --build build --config Release
```

The exact dependency installation and executable location will depend on the operating system, compiler, and CMake configuration.

Before distributing the application, provide tested build instructions for each supported platform.

## 10. Configuration

The application requires an API endpoint, an API key, and any provider-specific model configuration.

For example, the application may read an environment variable named:

```text
LLM_API_KEY
```

Set the variable in the user's environment before running the application. The exact API endpoint, authentication method, and model settings will depend on the chosen provider.

**Security requirements:**

* Never commit API keys or other credentials to Git.
* Add local secret and configuration files to `.gitignore`.
* Do not print API keys in terminal output or logs.
* Avoid embedding credentials directly in the executable.
* Treat source files and LLM responses as untrusted input.

## 11. Cross-Platform Compatibility

To improve portability:

* Use standard C++ filesystem and input/output APIs.
* Avoid depending on shell-specific commands such as `cls` or `clear`.
* Use portable path handling instead of hardcoded operating-system-specific paths.
* Use CMake to configure builds across supported platforms.
* Handle operating-system-specific behavior in isolated modules when unavoidable.
* Test builds and functionality on Windows, Linux, and macOS.

Cross-platform source code does not guarantee that one compiled executable will run on every operating system. Separate builds and platform-specific testing are required.

## 12. Error Handling and Reliability

The application should handle:

* Missing or unreadable source files.
* Invalid commands and command-line arguments.
* API authentication failures.
* Network timeouts and connection failures.
* API rate limits and unsuccessful HTTP responses.
* Invalid JSON or unexpected response formats.
* Large source files and context limits.
* Write failures and permission errors.
* Refactoring results that cannot be safely applied.

The application should report errors clearly and avoid modifying files when an operation fails.

## 13. Testing Strategy

Testing will cover:

* File reading and writing.
* Command parsing and argument validation.
* Prompt construction.
* JSON response parsing.
* API request and error handling.
* Code-generation output.
* Refactoring diff generation.
* Approval and file-saving behavior.
* Build and execution on supported operating systems.

Where possible, core modules should be tested independently of the external LLM API using mocked responses.

## 14. Future Enhancements

* Recursive project analysis with configurable file filters.
* Context management for multi-file projects.
* Git diff integration and optional Git-based rollback.
* Automated code-quality checks.
* Configurable LLM providers and models.
* Streaming responses in the terminal.
* Interactive review of individual changes.
* Automated cross-platform builds and release packages.
* Unit-test generation and documentation generation.

## 15. Limitations

* The application depends on the configured LLM provider and its availability.
* API usage may incur costs and is subject to provider rate limits.
* Generated code may contain errors or security vulnerabilities and must be reviewed.
* Large projects may require file selection, context reduction, or multiple requests.
* The tool suggests or applies textual changes; it does not guarantee that the resulting program compiles or behaves correctly.
* Cross-platform compatibility must be verified on actual target environments.

## 16. License

The project license has not yet been selected. Choose and document a license before distributing the application.

---

**Project summary:** A modular, cross-platform C++ terminal application that integrates an external LLM API to generate code, analyze source files, and propose reviewable refactoring changes while preserving user control over file modifications.
