# CodeAgent: AI Code Generation and Refactoring Assistant

CodeAgent is a C++17 assistant that uses Google's Gemini API to generate C++ code and to propose refactorings of existing source files, from the terminal or from a local browser GUI. It exists to make LLM help safe to use on real files: you always see the generated code or a diff first, nothing is written to disk without your explicit approval, and AI-generated code is never executed. CodeAgent calls an external model; it does not train or host one.

It has two front ends over one C++ core:

* a **terminal command**, `codeagent`, for generating code (and saving it) and for refactoring a file after reviewing a diff;
* a **browser GUI**, started with `codeagent gui`, with a chat, an editor, diff review, and Save/Save As. The GUI is served by a local C++ HTTP server.

Detailed design documentation: [docs/project-report.md](docs/project-report.md) (architecture, UML, OOP and C++ design, testing). UML diagrams: [UML-diagrams.md](UML-diagrams.md). Demonstration plan and viva preparation: [docs/demo-and-viva.md](docs/demo-and-viva.md). HTTP API: [docs/gui-api-contract.md](docs/gui-api-contract.md).

## Table of Contents

* [Key Features](#key-features)
  * [Project objectives](#project-objectives)
  * [Limitations](#limitations)
  * [Future enhancements](#future-enhancements)

* [Prerequisites & Tech Stack](#prerequisites--tech-stack)
  * [Supported platforms](#supported-platforms)
  * [Required software](#required-software)
  * [Tech stack](#tech-stack)
  * [System architecture](#system-architecture)
  * [Project structure](#project-structure)

* [Installation Guide](#installation-guide)
  * [Step 1: Clone the repository](#step-1-clone-the-repository)
  * [Step 2: Install the dependencies](#step-2-install-the-dependencies)
  * [Step 3: Install the `codeagent` command](#step-3-install-the-codeagent-command)
  * [Step 4: Configure the Gemini API key](#step-4-configure-the-gemini-api-key)
  * [Step 5: Verify the installation](#step-5-verify-the-installation)
  * [Build and test (development)](#build-and-test-development)
  * [Troubleshooting](#troubleshooting)

* [Usage Instructions](#usage-instructions)
  * [Terminal commands](#terminal-commands)
  * [Starting the GUI](#starting-the-gui)
  * [Environment variables](#environment-variables)
  * [Using the browser editor](#using-the-browser-editor)
  * [Error messages](#error-messages)
  * [Security](#security)

* [Credits / Contact](#credits--contact)

## Key Features

* **Code generation:** `codeagent generate "<instruction>"` and the GUI *Generate* mode. The terminal command offers to save the code to a path you choose, with overwrite confirmation.
* **Refactoring with diff review:** `codeagent refactor <file> "<instruction>"` shows a unified diff and writes the file only after `y`. The GUI creates a proposal; *Review changes* shows a colored diff; *Accept* puts it into the editor, *Reject* discards it.
* **Browser editor:** a line-numbered editor that scrolls up to 45 visible lines and keeps the complete document; open by path or dialog, Save (with external-change detection), Save As, Reload, Copy.
* **Chat:** prompts, Gemini replies with their thought summaries, decisions, and errors stay in the conversation; *Resend prompt* sends the last prompt again unchanged.
* **Safe file handling:** atomic writes through a temporary file and rename, supported C/C++ extensions only, 512 KiB editor limit, explicit overwrite confirmation everywhere.
* **Actionable errors:** a typed C++ exception hierarchy turns failures into specific messages that explain the cause and the next step, in the terminal and the browser.
* **Security:** loopback-only server, per-session token, origin and host checks, request size limits, redacted diagnostics; the API key never leaves the backend except to Gemini.
* **Runs from any directory:** `scripts/install.sh` installs a `codeagent` command with a documented configuration lookup.

### Project objectives

* Build a C++17 application that integrates an external LLM API (Gemini) for code generation and refactoring.
* Read local source files, combine them with user instructions into prompts, and parse the JSON responses.
* Protect the user's files: show generated code or a diff first, and write only after explicit approval.
* Report every failure (configuration, network, provider, parsing, file system) with a message that explains the cause and the next step, without exposing secrets.
* Offer both a terminal workflow and a local browser GUI over the same C++ core.
* Keep the core testable without network access, through injected transports and handlers.

### Limitations

* Requires the Gemini API and network access; usage is subject to Gemini's quotas and rate limits.
* Generated code may be wrong or insecure and must be reviewed; CodeAgent does not compile or run it.
* One file at a time, up to 512 KiB, C and C++ source extensions only.
* The editor is a plain text area without syntax highlighting.
* Linux/WSL2 only (see [Supported platforms](#supported-platforms)).
* The WSL PowerShell file dialog has not been exercised with a real click in automated tests.

### Future enhancements

* Multi-file and project-level refactoring with context management.
* Configurable model and provider.
* Streaming responses.
* Syntax highlighting in the editor.
* A packaged release and continuous integration builds.

## Prerequisites & Tech Stack

### Supported platforms

| Platform | Status |
| --- | --- |
| Ubuntu 24.04 under WSL2 | Built, unit-tested, installed, and smoke-tested |
| Other Linux distributions | Expected to work; not tested |
| Native Windows | Not supported (`setenv`; the Windows code paths have never been built) |
| macOS | Not tested; no native dialog integration |

A native Windows (MSVC/MinGW) build is not supported: `EnvLoader` uses POSIX `setenv`, and the Windows-specific code paths in the GUI server have never been built.

### Required software

* Linux or WSL2 (Ubuntu 24.04 tested), a C++17 compiler (GCC tested), CMake ≥ 3.15.
* libcurl and nlohmann/json development packages; internet access the first time CMake configures (to fetch GoogleTest and cpp-httplib).
* Node.js 18 or later for the frontend tests.
* Git, to clone the repository.
* A Gemini API key and an internet connection for generation and refactoring.

### Tech stack

| Component | Technology |
| --- | --- |
| Language | C++17 (backend, CLI); JavaScript ES modules, HTML, CSS (frontend, no framework) |
| Build | CMake ≥ 3.15 |
| HTTP client | libcurl |
| HTTP server | cpp-httplib v0.58.0 (fetched by CMake) |
| JSON | nlohmann/json |
| Files | `std::filesystem` and POSIX file APIs |
| Tests | GoogleTest v1.14.0 (fetched by CMake), Node.js built-in test runner |
| LLM | Google Gemini API, model `gemini-3.5-flash` |

### System architecture

```text
 Terminal ──► main ──► CLIController ──► PromptBuilder ──► LLMClient ──► Gemini API
                         │    │                              │
                         │    └─► FileManager (CLI files)    ├─► ResponseParser
                         │                                   └─► EnvLoader (configuration)
                         └─► codeagent gui ─► GuiHttpServer ◄──── browser frontend (JS)
                                               │  handlers supplied by CLIController
                                               ├─► UnifiedDiff, file I/O, proposals
                                               └─► NativeDialogProvider (zenity / PowerShell)
 All layers report failures as CodeAgentError subclasses (include/CodeAgentError.hpp).
```

The GUI server does not depend on `LLMClient`: `CLIController` gives it generation and refactoring handlers (`std::function`), which tests replace with fakes. The class responsibilities, UML class and sequence diagrams, and the design reasoning are in [docs/project-report.md](docs/project-report.md) and [UML-diagrams.md](UML-diagrams.md).

### Project structure

```text
LLM-CLI-Copilot-Integration/
├── include/            public headers (one class or module per file)
│   ├── CLIController.hpp      command dispatch, terminal workflows
│   ├── CodeAgentError.hpp     exception hierarchy and reporting helpers
│   ├── EnvLoader.hpp          .env loading and configuration lookup
│   ├── FileManager.hpp        CLI file reading and safe writing
│   ├── GuiHttpServer.hpp      local HTTP server for the GUI (pimpl)
│   ├── LLMClient.hpp          Gemini requests and failure classification
│   ├── NativeDialog.hpp       file/folder dialog interface, WSL path helpers
│   ├── PromptBuilder.hpp      generation and refactoring prompts
│   ├── ResponseParser.hpp     Gemini response and error parsing
│   └── UnifiedDiff.hpp        unified diff generation
├── src/                implementations, main.cpp, detail/FileReplacement.hpp
├── tests/              GoogleTest suites (one per component)
├── frontend/           index.html, css/, js/, tests/ (Node test runner)
├── scripts/            install.sh, smoke-test.sh
├── docs/               project report, demo and viva guide, API contract
├── CMakeLists.txt
├── UML-diagrams.md
└── README.md
```

## Installation Guide

### Step 1: Clone the repository

```bash
git clone https://github.com/LalitAdithya1/LLM-CLI-Copilot-Integration.git
cd LLM-CLI-Copilot-Integration
```

### Step 2: Install the dependencies

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev nlohmann-json3-dev nodejs
```

CMake downloads GoogleTest and cpp-httplib automatically the first time you configure. Node.js is only needed to run the frontend tests.

### Step 3: Install the `codeagent` command

1. Make the install script executable, then run it. Wait for it to finish: it configures a Release build, builds it, runs the whole test suite, and installs.

   ```bash
   chmod +x scripts/install.sh
   scripts/install.sh            # or, without chmod: bash scripts/install.sh
   ```

2. Add `~/.local/bin` to your `PATH` so the shell can find the command:

   ```bash
   echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
   ```

3. Reload your shell configuration (or open a new terminal):

   ```bash
   source ~/.bashrc
   ```

4. Check that the command is found:

   ```bash
   which codeagent               # prints /home/<you>/.local/bin/codeagent
   ```

The script builds in `build-release/` and installs into `~/.local` (no `sudo`):

```text
~/.local/bin/codeagent                 the command
~/.local/share/codeagent/frontend/     the GUI, found relative to the binary
```

Steps 2 and 3 are needed only once. The script prints whether `~/.local/bin` is already on your `PATH`; if it is, skip them.

Options: `--prefix DIR` installs elsewhere (for example `--prefix /usr/local` with `sudo`), `--skip-tests` skips the tests, and `--uninstall` removes exactly the files the last install created (your configuration is kept). Re-run the script after pulling changes.

A manual installation is equivalent to what the script does:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j2
cmake --install build-release --prefix ~/.local
```

The install contains only `bin/codeagent` and `share/codeagent/frontend/`; GoogleTest's own install rules are switched off.

### Step 4: Configure the Gemini API key

Store the key once per user so that `codeagent` finds it from any directory:

```bash
mkdir -p ~/.config/codeagent
printf 'GEMINI_API_KEY=%s\n' 'your-key-here' > ~/.config/codeagent/.env
chmod 600 ~/.config/codeagent/.env
```

`codeagent` looks for configuration in this order; the first value found for a variable wins:

1. variables already set in the environment (`export GEMINI_API_KEY=...`);
2. the file named by `CODEAGENT_ENV_FILE`, if set (a missing file is reported);
3. `.env` in the current directory, for per-project settings (git-ignored in this repository);
4. `$XDG_CONFIG_HOME/codeagent/.env`, or `~/.config/codeagent/.env` when `XDG_CONFIG_HOME` is unset.

A value may be wrapped in one pair of matching quotes (`GEMINI_API_KEY="..."`); the quotes are not part of the key. The files are read once per process, before the GUI server starts handling requests. If the key is missing, the error says so and lists the files that were checked. The key is sent only to Gemini, in the `x-goog-api-key` header. It is never printed, logged, or returned to the browser, and error diagnostics are redacted.

The endpoint and model (`gemini-3.5-flash`) are set in `src/LLMClient.cpp`; the request timeout is 60 seconds with a 10-second connection timeout.

### Step 5: Verify the installation

From **any** directory:

```bash
codeagent help
```

To check an installation end to end, run `bash scripts/smoke-test.sh`. It runs `codeagent` in a clean environment from a temporary folder outside the repository, checks configuration handling, sends Gemini a deliberately invalid key, and starts the GUI to check that it serves its installed frontend and API. It never uses your real key.

### Build and test (development)

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure   # 206 C++ tests in 20 suites
node --test frontend/tests/*.test.mjs        # 78 frontend tests
bash scripts/smoke-test.sh                    # installed command, end to end
```

During development you can also run the binary from the build tree, for example `./build/codeagent gui`.

The C++ tests replace the Gemini transport, the GUI handlers, the clock, and the native dialogs with test doubles, so they need no network and no API key. The frontend DOM integration test runs the real frontend modules against a small, dependency-free test DOM; it does not replace a browser test. The smoke test exercises the installed binary from a folder outside the repository, including one real request to Gemini with an invalid key. Details per suite are in [docs/project-report.md](docs/project-report.md#10-testing).

### Troubleshooting

| Symptom | Fix |
| --- | --- |
| `scripts/install.sh: Permission denied` | The script is not executable. Run `chmod +x scripts/install.sh`, or start it with `bash scripts/install.sh`. |
| `codeagent: command not found` | Run `scripts/install.sh`. If it reports that `~/.local/bin` is not on your `PATH`, run `echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc`, then `source ~/.bashrc`. `which codeagent` shows which binary runs. |
| Terminal: `Warning: GEMINI_API_KEY is not set`; browser or CLI: `Configuration error: GEMINI_API_KEY is not configured` | Put the key in `~/.config/codeagent/.env` (works from every folder) or export it, then restart. The CLI's `Details:` line lists the `.env` files that were checked. A `.env` in the repository is only read when you run `codeagent` from that folder. |
| `Warning: CODEAGENT_ENV_FILE is set to …, but that file could not be read` | Fix the path in `CODEAGENT_ENV_FILE`, or unset it. |
| Browser or CLI: `Authentication failed`, `Request rate limited`, `Network error`, `Request timed out` | The message names the cause and the next step. The terminal running `codeagent` also logs a redacted diagnostic, such as the HTTP status and Gemini's error reason; API keys are never printed. |
| `Could not start the GUI server on 127.0.0.1:8080` | Another program (often another `codeagent gui`) is using the port. Stop it, or run with `CODEAGENT_GUI_PORT=8081`. |
| `Could not open a browser automatically` | Open the printed one-time link (`http://127.0.0.1:<port>/#bootstrap=…`) within 5 minutes. The server keeps running. |
| The page says *Not connected* / `SESSION_REQUIRED` or `INVALID_BOOTSTRAP` | The page was opened without a valid one-time link (typed address, other browser, link used twice or older than 5 minutes), or the server was restarted. Restart `codeagent gui` and use the link it opens or prints. |
| `Frontend files were not found; only the API is available` | Set `CODEAGENT_FRONTEND_DIR=/path/to/repo/frontend`. |
| *Choose source path* / *Choose output directory* say no dialog is available | On Linux, install zenity (`sudo apt install zenity`) and run in a graphical session. Under WSL, the Windows dialog needs `powershell.exe` to be reachable through WSL interop. Either way, typing the path and selecting *Open path* / *Set directory* always works. |
| The dialog seems not to open | Under WSL the Windows dialog can open behind the browser window; check the taskbar. It closes itself after 2 minutes. |
| *Save* says `FILE_NOT_OPENED` | Only files opened by path in this session can be saved in place. Use *Save As*, or open the file by path. |

## Usage Instructions

### Terminal commands

```bash
codeagent help
codeagent generate "Implement binary search in C++"
codeagent refactor src/example.cpp "Improve readability"
codeagent gui
```

The CLI commands are `help` (also `-h`, `--help`), `gui`, `generate "<instruction>"`, and `refactor <file> "<instruction>"`. Unknown commands and missing arguments print a usage error and exit with status 1.

After `generate` prints the code, it asks `Do you want to save the generated code to a file? (y/n):`. Answer `y` and enter a path ending in a C/C++ source extension (`.cpp`, `.hpp`, …) inside an existing folder. An empty or invalid path is explained and asked again; an existing file is only replaced after you confirm. Answering `n`, or ending input, saves nothing.

`refactor` prints a unified diff and writes the file only after you answer `y`.

### Starting the GUI

`codeagent gui` is the only process you need to start. It:

1. Starts the C++ server on `127.0.0.1:8080`. It binds to loopback only, never to `0.0.0.0`.
2. Serves the browser frontend (HTML/CSS/JS) and the JSON API from that same origin.
3. Opens your default browser: `wslview` or `rundll32.exe url.dll,FileProtocolHandler` under WSL, `xdg-open` on Linux. The address carries a one-time startup code (`#bootstrap=…`), which the page exchanges for the API session token (see [Security](#security)). If no browser can be opened, or `CODEAGENT_NO_BROWSER` is set, it prints a one-time link instead. Open it within 5 minutes.
4. Runs until you press **Ctrl+C**. There is no route to stop the server.

The folder you start it from becomes the default output directory for Save As.

Reloading the page or opening the address again in a new tab of the same browser reconnects on its own. Another browser needs a new link: restart `codeagent gui`.

`codeagent` finds the frontend without depending on your working directory. It checks, in order: `CODEAGENT_FRONTEND_DIR`; `frontend/` next to the executable or one level above it (the build tree); `../share/codeagent/frontend` relative to the executable (the installed layout); and finally the source tree it was built from. At startup it prints `Serving frontend from <folder>`.

### Environment variables

| Environment variable | Purpose |
| --- | --- |
| `GEMINI_API_KEY` | Gemini API key (see the [lookup order](#step-4-configure-the-gemini-api-key)). |
| `CODEAGENT_ENV_FILE` | A specific `.env` file to read before the others. |
| `CODEAGENT_GUI_PORT` | Use another port, e.g. `CODEAGENT_GUI_PORT=8081 codeagent gui`. |
| `CODEAGENT_NO_BROWSER` | Set to `1` to skip opening a browser (headless/SSH use). |
| `CODEAGENT_FRONTEND_DIR` | Directory containing `index.html`, if the frontend is not found automatically. |
| `CODEAGENT_DEV_ORIGIN` | Development only: also accept a frontend served from another loopback origin, exactly `http://127.0.0.1:<port>` or `http://localhost:<port>` (e.g. `http://127.0.0.1:5500`). A one-time link for it is printed at startup. Unset by default. |

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
* **Resend prompt**, next to the submit button, sends your last prompt to Gemini again exactly as it was submitted (same instruction, language, and mode), through the same request as the submit button. The reply appears as a new exchange below the earlier ones. It is disabled until a prompt has been sent and while a request is running; it also retries a request that failed. A resent refactoring works on the current editor contents, and like any request it waits until a pending proposal has been accepted or rejected. The prompt field is left as it is, so a draft you are typing is kept.
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

### Error messages

Every failure that the user can act on is thrown as a subclass of `CodeAgentError` (`include/CodeAgentError.hpp`) and caught at a boundary that reports it: `CLIController::run` for the terminal (exit status 1), the GUI route handlers (JSON error with a specific status and code), and `main` as a last resort. Examples of messages:

| Situation | Message |
| --- | --- |
| No API key | `Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file.` |
| Rejected key (HTTP 401/403, or 400 `API_KEY_INVALID`) | `Authentication failed: Gemini rejected the configured credentials. Verify the API key.` |
| HTTP 429 without a daily-quota violation | `Request rate limited. Wait briefly before retrying.` (plus Gemini's suggested delay when given) |
| Connection failure / timeout | `Network error: Unable to connect to Gemini. Check your connection.` / `Request timed out. Retry when the service is reachable.` |
| Gemini 5xx | `Gemini is temporarily unavailable. Retry in a moment.` |
| No write permission | `Permission denied: CodeAgent cannot access this file. …` (GUI) / `Permission denied: CodeAgent cannot write to <path>. …` (CLI) |
| File changed since it was opened | `The file changed externally. Reload it or save to another path.` |

The design, the propagation paths, and the RAII resource handling are described in [docs/project-report.md](docs/project-report.md#7-exception-handling-design).

### Security

Security rules the implementation follows:

* API keys are never committed (`.env` and `.codeagent/` are git-ignored), printed, logged, or sent to the browser.
* Diagnostics are passed through `redactSecrets`, which removes the configured key, Google-style keys, and key-carrying URL parameters and headers.
* Gemini's free-text error messages are never shown or logged; only validated, structured fields (status, reason, quota identifiers, retry delay) are.
* Source files and LLM responses are treated as untrusted input; generated code is never executed and is rendered in the browser as text, never as HTML.

**Session token.** Each server instance generates a random 256-bit session token at startup from the operating system's cryptographic random source (`getrandom`). Every `/api/*` request must send it in the `X-CodeAgent-Token` header, or it gets `401 UNAUTHORIZED` before any route runs: no file is read or written, no dialog opens, no proposal changes, and Gemini is not called. The comparison takes the same time however many characters match. The token is kept in memory only. It is never printed, logged, or returned by any route except `POST /api/session`.

**How the page gets the token.** `codeagent gui` creates a one-time *bootstrap code* (also 256 bits, valid 5 minutes, single use) and opens the browser at `http://127.0.0.1:<port>/#bootstrap=<code>`. The code is in the URL fragment, which browsers never send to the server or in a `Referer`. The page reads it, removes it from the address bar and history, and posts it to `POST /api/session`. That route requires an allowed `Origin`. It returns the token, which the page keeps in a JavaScript variable (not `localStorage` or `sessionStorage`). It also sets an `HttpOnly; SameSite=Strict` cookie restricted to `/api/session`, which holds a separate secret. After a reload the page posts to `/api/session` without a code, and the cookie gets it the token again. The cookie alone never authorizes an API call; only the header does.

**Origins.** The API accepts browser requests only from the page it serves: `http://127.0.0.1:<port>` and `http://localhost:<port>`. Development origins are no longer allowed by default. Set `CODEAGENT_DEV_ORIGIN` to one exact loopback origin to allow it, for example `CODEAGENT_DEV_ORIGIN=http://127.0.0.1:5500` for a static server on port 5500. The page then calls the API on port 8080, and a one-time link for that origin is printed. Other origins, wildcards, and `Origin: null` are rejected. CORS preflights from an allowed origin may announce only `Content-Type` and `X-CodeAgent-Token`. Browsers never attach custom headers to a preflight, so `OPTIONS` requests cannot carry the token. A preflight therefore performs no action and returns no data; it only permits the authenticated request that follows.

Every API request must also carry a `Host` of `127.0.0.1:<port>` or `localhost:<port>`, which blocks DNS rebinding. Request bodies are limited to 1 MiB. Static files are served with `X-Content-Type-Options: nosniff`, `Cache-Control: no-store`, and a same-origin `Content-Security-Policy`. cpp-httplib rejects `..` traversal and symlinks that escape the frontend directory. The frontend's static files contain no secrets.

**Limits of this protection.** The token stops other websites and other local users' processes from using the API, as long as they cannot read your files or memory. It does not protect against malicious software running as your own user. Such software can read the browser's memory or profile, the terminal output, or attach to the server process. The bootstrap code is passed to the browser launcher on its command line, where other local users can briefly see it in the process list until the page uses it. It is also printed when no browser can be opened. Both cases are limited by the code being single-use and expiring after 5 minutes.

**File access.** The backend reads a file only when the user explicitly opens it by path or through the dialog (`POST /api/files/read`). Only regular files with a supported source extension, up to 512 KiB of UTF-8 text, are accepted. Symbolic links must resolve to a supported source file. The size limit is enforced while reading, so a file that grows after the check cannot be read beyond it, and FIFOs and devices are refused without blocking. The 256 most recently opened or saved files can be saved in place; beyond that, the least recently used one must be opened again. It writes back in place only to files opened that way in the current session (`POST /api/files/write`), checking that the file has not changed since; every other write goes through the output-directory rules above. Settings is a disabled placeholder.

**Proposals.** The backend retains at most 128 proposals and in-flight creation reservations combined. Refactor capacity is reserved before provider work begins. Proposals are only removed when a new one needs the slot. The backend then picks one in this order:

1. The oldest rejected or applied proposal. A proposal counts as applied once it has been saved, or once the browser has put it into the editor (`mark-applied`).
2. Otherwise, the pending or approved proposal that has been idle longest, if it has been idle for at least **30 minutes**. Idle means no diff, approve, reject, mark-applied, or save request has used it. This reclaims proposals abandoned by a page reload.

Proposals reserved for another in-flight request are skipped. The choice is made, and checked again when the new proposal is stored, under the same lock that every proposal request and the proposal save hold. A proposal is therefore never removed while it is being saved or changing state. If a proposal reserved for replacement was used again while Gemini was working, another eligible proposal is replaced instead, or the request fails. If no slot is available, the request receives `503 PROPOSAL_CAPACITY`, without invoking the provider when this is known up front. An idle proposal stays fully usable until it is actually replaced. Failed provider or response-preparation work releases its reservation and keeps the proposal selected for replacement.

**Native dialogs.** On POSIX systems, native dialogs use `zenity` when it is installed and a graphical `DISPLAY` or `WAYLAND_DISPLAY` exists. Under WSL without zenity, they use a Windows Forms dialog started through `powershell.exe`, and the returned Windows path is translated to its WSL path. Dialogs time out after 2 minutes. A native Windows build reports dialogs unavailable. macOS has no native dialog integration. Automated tests cover dialog outcomes through an injected provider. The WSL PowerShell dialog has not been exercised with a real click in automated testing; only its launch, path translation, and script syntax have been verified.

## Credits / Contact

CodeAgent was built by:

* **Lalitadithya Kariyam**, Indian Institute of Information Technology (IIIT) Tiruchirappalli
* **Samuel John Dasi**, Indian Institute of Information Technology (IIIT) Tiruchirappalli

Source code, issues, and questions: [github.com/LalitAdithya1/LLM-CLI-Copilot-Integration](https://github.com/LalitAdithya1/LLM-CLI-Copilot-Integration).

Built with [Google Gemini](https://ai.google.dev/), [cpp-httplib](https://github.com/yhirose/cpp-httplib), [nlohmann/json](https://github.com/nlohmann/json), [libcurl](https://curl.se/libcurl/), and [GoogleTest](https://github.com/google/googletest).

**License:** released under the [MIT License](LICENSE).
