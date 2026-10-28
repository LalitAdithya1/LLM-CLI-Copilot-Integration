# AI Code Generation and Refactoring Assistant

A cross-platform, terminal-based C++ application that uses a Large Language Model (LLM) API to generate code, analyze existing source files, and suggest refactoring improvements.

The application provides a command-line interface (CLI) for interacting with an LLM, managing source files, and reviewing AI-generated code changes. It is designed to work across Windows, Linux, and macOS with a shared C++ codebase.

## 1. Project Overview

The AI Code Generation and Refactoring Assistant helps developers improve their productivity by integrating AI-powered code generation and refactoring into a terminal workflow.

Users can provide instructions to generate new code, analyze existing code for potential improvements, and review suggested changes before applying them to their projects.

The application communicates with an external LLM API. It does not train or host its own language model.

## Quick Start

Supported and tested platform: **Linux, including WSL2 on Windows (Ubuntu 24.04)**. A native Windows (MSVC/MinGW) build is not supported: `EnvLoader` uses POSIX `setenv`. macOS is untested.

### Dependencies

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev nlohmann-json3-dev nodejs
```

CMake downloads GoogleTest and cpp-httplib automatically the first time you configure. Node.js is only needed to run the frontend tests.

### Configure the Gemini API key

Create a `.env` file in the directory you launch `codeagent` from. The file is git-ignored.

```text
GEMINI_API_KEY=your-key-here
```

You can also `export GEMINI_API_KEY=...`. Existing environment variables take precedence over `.env`. A value may be wrapped in one pair of matching quotes (`GEMINI_API_KEY="..."`); the quotes are not part of the key. `.env` is read once per process, before the GUI server starts handling requests. The key is sent only to Gemini in the `x-goog-api-key` header. It is never printed, logged, or returned to the browser. The model is `gemini-3.5-flash`.

### Build and test

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
node --test frontend/tests/*.test.mjs
```

### Run

```bash
./build/codeagent help
./build/codeagent generate "Implement binary search in C++"
./build/codeagent refactor src/example.cpp "Improve readability"
./build/codeagent gui
```

`codeagent gui` is the only process you need to start. It:

1. Starts the C++ server on `127.0.0.1:8080`. It binds to loopback only, never to `0.0.0.0`.
2. Serves the browser frontend (HTML/CSS/JS) and the JSON API from that same origin.
3. Opens your default browser: `wslview` or `rundll32.exe url.dll,FileProtocolHandler` under WSL, `xdg-open` on Linux. The address carries a one-time startup code (`#bootstrap=…`), which the page exchanges for the API session token (see [GUI server security](#gui-server-security)). If no browser can be opened, or `CODEAGENT_NO_BROWSER` is set, it prints a one-time link instead. Open it within 5 minutes.
4. Runs until you press **Ctrl+C**.

Reloading the page or opening the address again in a new tab of the same browser reconnects on its own. Another browser needs a new link: restart `codeagent gui`.

| Environment variable | Purpose |
| --- | --- |
| `GEMINI_API_KEY` | Gemini API key (or set it in `./.env`). |
| `CODEAGENT_GUI_PORT` | Use another port, e.g. `CODEAGENT_GUI_PORT=8081 ./build/codeagent gui`. |
| `CODEAGENT_NO_BROWSER` | Set to `1` to skip opening a browser (headless/SSH use). |
| `CODEAGENT_FRONTEND_DIR` | Directory containing `index.html`, if the frontend is not found automatically. |
| `CODEAGENT_DEV_ORIGIN` | Development only: also accept a frontend served from another loopback origin, exactly `http://127.0.0.1:<port>` or `http://localhost:<port>` (e.g. `http://127.0.0.1:5500`). A one-time link for it is printed at startup. Unset by default. |

`codeagent` finds the frontend without depending on your working directory. It checks, in order: `CODEAGENT_FRONTEND_DIR`; `frontend/` next to the executable or one level above it (the build tree); `../share/codeagent/frontend` (after `cmake --install build`); and finally the source tree it was built from.

### Using the browser editor

The top of the page switches between two modes, **Generate** and **Refactor**. The *Source file* controls appear only in Refactor mode; switching modes keeps the editor contents and the source path. The AI Assistant and the editor sit side by side with matching heights (stacked on narrow windows). The editor shows up to 45 lines and scrolls for longer files; the chat scrolls inside its card.

* **Open a file to edit it in place** (in Refactor mode).
  * Type its path in *Source file* and select *Open path* (or press Enter). Both WSL paths (`/mnt/d/project/insertion_sort.cpp`) and Windows paths (`D:\project\insertion_sort.cpp`, `\\wsl.localhost\Ubuntu\home\me\x.cpp`) work; under WSL the backend translates Windows paths.
  * Alternatively, select *Choose source path* to pick the file in a native dialog. It uses zenity when installed; under WSL without zenity it opens a Windows file dialog through `powershell.exe`, which may appear behind the browser. If no dialog is available, the page says so and focuses the path field.
  * The editor heading shows the file name. A `●` and *Unsaved changes* mark edits that are not yet on disk. A new document is shown as `untitled.cpp`.
* **Save** (or Ctrl+S) writes the editor back to the file you opened, atomically. Its read/write/execute permission bits are kept; setuid, setgid, and sticky bits are cleared.
  * If the file changed on disk since you opened or last saved it (for example, you saved it in VS Code), Save asks before overwriting. *Reload* re-reads the file from disk.
  * Opening another file, reloading, or uploading over unsaved edits asks for confirmation first.
* **Save As** asks for a filename and writes it to the active output directory, asking before replacing an existing file. Once the backend confirms the save, the editor *is* the new file: its name, the *Source file* path, Save, Reload, and change detection all use the new path. The original file is not modified. If the save fails or you cancel, the editor keeps the current file. If the saved file cannot be reopened for in-place saving, the page says so; open it by path to keep editing it.
* *Open file in editor* loads a copy through the browser file picker. Browsers do not reveal where such a file lives, so it can only be written with Save As.
* *Copy* copies the current editor text. The button shows *Copied!* with a check mark for about 2 seconds once the copy succeeded. It uses the Clipboard API and falls back to the browser's copy command; if both fail, the button keeps saying *Copy* and the failure is reported next to the editor.
* **Generate:** enter a language and an instruction, then select *Generate code*. Gemini's output is shown for review.
  * *Accept* puts it into the editor (replacing the editor contents) to edit and save with Save or Save As.
  * *Reject* discards it.
* **Refactor:** with code in the editor, switch to *Refactor* and describe the change. The backend creates a **proposal**. Select *Review changes* to load the unified diff, colored like `git diff`: removed lines red, added lines green, headers blue.
  * *Accept* replaces the editor contents with the proposed code. Nothing is written to disk: the document is marked unsaved, you can keep editing, and Save (Ctrl+S) or Save As writes the final editor contents.
  * If the editor has unsaved changes, Accept asks first. A proposal is applied only if it is still pending and the editor still contains the code it was created from.
  * *Reject* leaves the editor and the files on disk unchanged.
* **Save As** writes `<active output directory>/<filename>`.
  * The output directory defaults to the launch directory. Type a WSL or Windows path and select *Set directory*, or use *Choose output directory* for a native dialog. The backend accepts only existing, writable directories, and the active directory is always displayed.
  * Filenames must be plain names with a supported extension (see [Supported files](#supported-files)). Paths and `..` are rejected.
  * Replacing an existing file always asks for confirmation.
  * Writes are atomic: the content goes to a temporary file that is then renamed into place.
  * Buttons are disabled while a request is in flight, so double clicks cannot approve or save twice.
* Nothing is saved without an explicit Save or Save As. AI-generated code is **never executed**.
* The prompts ask Gemini for raw code. If a complete response is nevertheless one Markdown code block (`` ```cpp `` … `` ``` ``), the CLI and GUI receive only the code inside it. Any other response, including partial output or text around a code block, is shown exactly as returned.

#### Supported files

The backend opens, saves in place, and writes only C and C++ source files: `.c .cc .cpp .cxx .h .hh .hpp .hxx`. It enforces this on every open and save; the page no longer lists the extensions, and *Open file in editor* checks the same list before loading a file. Files are limited to 512 KiB of UTF-8 text.

#### Editing the same file in VS Code

Save writes the file on disk. It does not talk to VS Code. VS Code watches files, so a file open there without unsaved changes normally reloads to show the new contents. If it has unsaved changes, VS Code keeps your buffer and reports a conflict when you save it there. In the other direction, edits saved in VS Code are not pushed into the browser; select *Reload*. If you save in the browser without reloading, the backend detects the change and asks before overwriting it. This is not live two-way synchronization.

### Troubleshooting

| Symptom | Fix |
| --- | --- |
| Terminal: `Warning: GEMINI_API_KEY is not set`; browser: `GENERATION_FAILED` / `REFACTOR_FAILED` | Add the key to `.env` in the launch directory or export it, then restart. The terminal running `codeagent` prints the specific cause (missing key, HTTP status, network error); the browser intentionally shows only a generic message. |
| `Could not start the GUI server on 127.0.0.1:8080` | Another program (often another `codeagent gui`) is using the port. Stop it, or run with `CODEAGENT_GUI_PORT=8081`. |
| `Could not open a browser automatically` | Open the printed one-time link (`http://127.0.0.1:<port>/#bootstrap=…`) within 5 minutes. The server keeps running. |
| The page says *Not connected* / `SESSION_REQUIRED` or `INVALID_BOOTSTRAP` | The page was opened without a valid one-time link (typed address, other browser, link used twice or older than 5 minutes), or the server was restarted. Restart `codeagent gui` and use the link it opens or prints. |
| `Frontend files were not found; only the API is available` | Set `CODEAGENT_FRONTEND_DIR=/path/to/repo/frontend`. |
| *Choose source path* / *Choose output directory* say no dialog is available | On Linux, install zenity (`sudo apt install zenity`) and run in a graphical session. Under WSL, the Windows dialog needs `powershell.exe` to be reachable through WSL interop. Either way, typing the path and selecting *Open path* / *Set directory* always works. |
| The dialog seems not to open | Under WSL the Windows dialog can open behind the browser window; check the taskbar. It closes itself after 2 minutes. |
| *Save* says `FILE_NOT_OPENED` | Only files opened by path in this session can be saved in place. Use *Save As*, or open the file by path. |

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
* Support commands such as `gui`, `generate`, `refactor`, `help`, and `exit`.
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

## 7. Usage Details

The CLI commands are `help`, `gui`, `generate "<instruction>"`, and `refactor <file> "<instruction>"`. The CLI `generate` prints the code without saving it. The CLI `refactor` prints a unified diff and writes the file only after you answer `y`. See the Quick Start for the GUI workflow.

### GUI server security

**Session token.** Each server instance generates a random 256-bit session token at startup from the operating system's cryptographic random source (`getrandom`). Every `/api/*` request must send it in the `X-CodeAgent-Token` header, or it gets `401 UNAUTHORIZED` before any route runs: no file is read or written, no dialog opens, no proposal changes, and Gemini is not called. The comparison takes the same time however many characters match. The token is kept in memory only. It is never printed, logged, or returned by any route except `POST /api/session`.

**How the page gets the token.** `codeagent gui` creates a one-time *bootstrap code* (also 256 bits, valid 5 minutes, single use) and opens the browser at `http://127.0.0.1:<port>/#bootstrap=<code>`. The code is in the URL fragment, which browsers never send to the server or in a `Referer`. The page reads it, removes it from the address bar and history, and posts it to `POST /api/session`. That route requires an allowed `Origin`. It returns the token, which the page keeps in a JavaScript variable (not `localStorage` or `sessionStorage`). It also sets an `HttpOnly; SameSite=Strict` cookie restricted to `/api/session`, which holds a separate secret. After a reload the page posts to `/api/session` without a code, and the cookie gets it the token again. The cookie alone never authorizes an API call; only the header does.

**Origins.** The API accepts browser requests only from the page it serves: `http://127.0.0.1:<port>` and `http://localhost:<port>`. Development origins are no longer allowed by default. Set `CODEAGENT_DEV_ORIGIN` to one exact loopback origin to allow it, for example `CODEAGENT_DEV_ORIGIN=http://127.0.0.1:5500` for a static server on port 5500. The page then calls the API on port 8080, and a one-time link for that origin is printed. Other origins, wildcards, and `Origin: null` are rejected. CORS preflights from an allowed origin may announce only `Content-Type` and `X-CodeAgent-Token`. Browsers never attach custom headers to a preflight, so `OPTIONS` requests cannot carry the token. A preflight therefore performs no action and returns no data; it only permits the authenticated request that follows.

Every API request must also carry a `Host` of `127.0.0.1:<port>` or `localhost:<port>`, which blocks DNS rebinding. Request bodies are limited to 1 MiB. Static files are served with `X-Content-Type-Options: nosniff`, `Cache-Control: no-store`, and a same-origin `Content-Security-Policy`. cpp-httplib rejects `..` traversal and symlinks that escape the frontend directory. The frontend's static files contain no secrets.

**Limits of this protection.** The token stops other websites and other local users' processes from using the API, as long as they cannot read your files or memory. It does not protect against malicious software running as your own user. Such software can read the browser's memory or profile, the terminal output, or attach to the server process. The bootstrap code is passed to the browser launcher on its command line, where other local users can briefly see it in the process list until the page uses it. It is also printed when no browser can be opened. Both cases are limited by the code being single-use and expiring after 5 minutes.

The backend reads a file only when the user explicitly opens it by path or through the dialog (`POST /api/files/read`). Only regular files with a supported source extension, up to 512 KiB of UTF-8 text, are accepted. Symbolic links must resolve to a supported source file. The size limit is enforced while reading, so a file that grows after the check cannot be read beyond it, and FIFOs and devices are refused without blocking. The 256 most recently opened or saved files can be saved in place; beyond that, the least recently used one must be opened again. It writes back in place only to files opened that way in the current session (`POST /api/files/write`), checking that the file has not changed since; every other write goes through the output-directory rules above. Settings is a disabled placeholder.

The backend retains at most 128 proposals and in-flight creation reservations combined. Refactor capacity is reserved before provider work begins. Proposals are only removed when a new one needs the slot. The backend then picks one in this order:

1. The oldest rejected or applied proposal. A proposal counts as applied once it has been saved, or once the browser has put it into the editor (`mark-applied`).
2. Otherwise, the pending or approved proposal that has been idle longest, if it has been idle for at least **30 minutes**. Idle means no diff, approve, reject, mark-applied, or save request has used it. This reclaims proposals abandoned by a page reload.

Proposals reserved for another in-flight request are skipped. The choice is made, and checked again when the new proposal is stored, under the same lock that every proposal request and the proposal save hold. A proposal is therefore never removed while it is being saved or changing state. If a proposal reserved for replacement was used again while Gemini was working, another eligible proposal is replaced instead, or the request fails. If no slot is available, the request receives `503 PROPOSAL_CAPACITY`, without invoking the provider when this is known up front. An idle proposal stays fully usable until it is actually replaced. Failed provider or response-preparation work releases its reservation and keeps the proposal selected for replacement.

On POSIX systems, native dialogs use `zenity` when it is installed and a graphical `DISPLAY` or `WAYLAND_DISPLAY` exists. Under WSL without zenity, they use a Windows Forms dialog started through `powershell.exe`, and the returned Windows path is translated to its WSL path. Dialogs time out after 2 minutes. A native Windows build reports dialogs unavailable. macOS has no native dialog integration. Automated tests cover dialog outcomes through an injected provider. The WSL PowerShell dialog has not been exercised with a real click in automated testing; only its launch, path translation, and script syntax have been verified.

There is no route to stop the server. Stop the backend with Ctrl+C in its terminal.

Run the frontend checks with:

```bash
node --test frontend/tests/*.test.mjs
```

The app DOM integration test runs the real frontend module against a small, dependency-free test DOM. It verifies the HTML selectors and the event handlers for file loading, generation, review, approve, reject, and save. It does not replace a browser smoke test.

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

The application reads the Gemini key from `GEMINI_API_KEY`, either from the environment or from a `.env` file in the launch directory. The endpoint and model (`gemini-3.5-flash`) are set in `src/LLMClient.cpp`.

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
