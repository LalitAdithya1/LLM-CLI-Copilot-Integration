# CodeAgent — Demonstration Checklist and Viva Preparation

Use this together with [project-report.md](project-report.md). Steps marked **(live)** call Gemini with your real key and need network access; everything else works offline.

## Before the demonstration

- [ ] `scripts/install.sh` has run successfully (it builds, runs all tests, and installs into `~/.local`).
- [ ] The key is in `~/.config/codeagent/.env` with `chmod 600` (see the README). Do not show the file's contents on screen.
- [ ] A fresh terminal finds the command: `command -v codeagent` prints `~/.local/bin/codeagent`.
- [ ] A demo folder outside the repository exists: `mkdir -p ~/codeagent-demo`.
- [ ] Port 8080 is free, or use `CODEAGENT_GUI_PORT=8081`.
- [ ] A browser is available (under WSL, `codeagent gui` opens the Windows default browser).

## Demonstration steps

### 1. Run `codeagent` from outside the repository

```bash
cd ~/codeagent-demo
command -v codeagent          # ~/.local/bin/codeagent
codeagent help                # usage, environment variables, configuration lookup order
```

Point out: the binary and its GUI files live under `~/.local`; the key comes from `~/.config/codeagent/.env`, so no repository files are needed.

### 2. Generate code and save it to a path **(live)**

```bash
codeagent generate "a function that checks whether a string is a palindrome"
```

1. The code is printed, then `Do you want to save the generated code to a file? (y/n):` → `y`.
2. `Enter the output file path:` → press Enter (empty) → explained, asked again.
3. Enter `notes.txt` → rejected (not a C/C++ source name), asked again.
4. Enter `palindrome.cpp` → `Generated code saved successfully to: /home/<you>/codeagent-demo/palindrome.cpp`.
5. Run the same command again and enter `palindrome.cpp` → `Overwrite this file? [y/N]` → `n` → the file is kept and you can enter another name.

### 3. Invalid configuration and API failures

```bash
GEMINI_API_KEY= codeagent generate "hello world"
#   Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file.
#   Details: GEMINI_API_KEY is unset or empty; .env files checked: …

GEMINI_API_KEY=not-a-real-key codeagent generate "hello world"        # (live)
#   Authentication failed: Gemini rejected the configured credentials. Verify the API key.
#   Details: Gemini HTTP 400; provider error status INVALID_ARGUMENT, reason API_KEY_INVALID

CODEAGENT_GUI_PORT=99999 codeagent gui
#   Configuration error: CODEAGENT_GUI_PORT must be a port number from 1 to 65535. …
```

A variable in the environment, even an empty one, takes precedence over the `.env` files, which is why the first command fails although the key is configured. Point out that no command crashes, the exit status is 1, and the key is never printed. Rate limits, timeouts, and unreachable networks cannot be triggered on demand; show their tests instead (`LLMClientTest.Http429IsARateLimitUnlessADailyQuotaIsExhausted`, `TransportExceptionsPropagateAndFailuresBecomeNetworkErrors`).

### 4. GUI: generation, chat history, Resend Prompt **(live)**

```bash
cd ~/codeagent-demo && codeagent gui
```

1. The browser opens with the Neutral Elegance theme. The status shows *Backend connected*.
2. Generate mode, language `C++`, instruction "binary search over a sorted vector" → *Generate code*. The chat shows the prompt, the progress state, Gemini's thoughts, and the code.
3. *Resend prompt* → the same prompt is sent again and a second exchange appears below the first.
4. *Accept* puts the code into the editor (marked unsaved). *Save As* → `search.cpp` writes it to `~/codeagent-demo`.

### 5. Editing, scrolling, diff review, accept/reject, saving **(live)**

1. Switch to *Refactor*. Type the path of `palindrome.cpp` and select *Open path*.
2. Scroll the editor: it shows up to 45 lines and keeps the whole file.
3. Instruction "use std::equal and add comments" → *Create proposal* → *Review changes*: removed lines red, added lines green, headers blue.
4. *Reject* → editor and file unchanged. Create the proposal again → *Accept* → the editor holds the proposed code, unsaved.
5. *Save* (Ctrl+S) writes it back. Change the file in another editor, then *Save* in the browser → it asks before overwriting (`The file changed externally. Reload it or save to another path.`).
6. Ctrl+C in the terminal stops the server.

### 6. Explain the design

| Topic | Show |
| --- | --- |
| UML and responsibilities | [project-report.md §4–5](project-report.md#4-architecture) |
| Runtime polymorphism | `respondWithError` in `src/GuiHttpServer.cpp` (one function for every `CodeAgentError`); `NativeDialogProvider::choose` |
| Exception propagation | Sequence diagram §6.3; `throwHttpError` in `src/LLMClient.cpp`; `CLIController::run` |
| Memory and resources | `CurlHandle`/`CurlHeaderList` in `src/LLMClient.cpp`; `TemporaryFileGuard` in `src/FileManager.cpp`; pimpl `std::unique_ptr<Impl>` |

### 7. Automated tests

```bash
cd /path/to/LLM-CLI-Copilot-Integration
ctest --test-dir build --output-on-failure      # 206 C++ tests
node --test frontend/tests/*.test.mjs           # 78 frontend tests
scripts/smoke-test.sh                           # installed command, outside the repository
```

## Likely viva questions

**1. How does `codeagent` run from any directory?**
`scripts/install.sh` runs `cmake --install` into `~/.local`: the binary goes to `~/.local/bin`, which Ubuntu's `~/.profile` puts on `PATH`, and the frontend to `~/.local/share/codeagent/frontend`. The binary finds the frontend relative to its own location (`/proc/self/exe`), not the working directory, and reads the key from `~/.config/codeagent/.env`. `scripts/smoke-test.sh` checks all of this from a clean shell in a temporary folder.

**2. In what order is configuration read, and why does the first value win?**
Environment, then `CODEAGENT_ENV_FILE`, then `./.env`, then `~/.config/codeagent/.env`. `loadEnvFile` never overwrites an existing variable, so loading the files in priority order makes the first source win. This lets a shell export or a project `.env` override the user default. The files are loaded once, before the server starts threads, because `setenv` is not thread-safe.

**3. Why exceptions instead of return codes?**
A failure such as an invalid key is detected deep in `LLMClient` but can only be reported by the CLI or by a GUI route. An exception carries the specific type and message up through the prompt builder and the handler `std::function` without every layer having to forward an error code. Expected outcomes that the caller must handle immediately — a declined overwrite, an invalid path the user typed — are returned as `FileManager::WriteResult` instead.

**4. Where are exceptions thrown and caught?**
Thrown where the failure is detected: `requireEnvironmentVariable`, the libcurl transport, `throwHttpError`, `LLMClient::generate`, `throwFileSystemError`, and the GUI save route. Caught only at boundaries that can report: `CLIController::run`, `main`, each GUI route, the server's exception handler, `FileManager::saveFile`, and the legacy `bool` methods of `LLMClient`.

**5. How do you avoid mislabelling every HTTP 429 as an exhausted quota?**
`ResponseParser::parseProviderError` reads Gemini's `QuotaFailure` details. Only a violated per-day quota (`quotaId` containing `PerDay`) produces `PROVIDER_QUOTA_EXHAUSTED`; any other 429 is `PROVIDER_RATE_LIMITED`, with Gemini's `RetryInfo` delay in the message. An invalid key arrives as HTTP 400 with reason `API_KEY_INVALID`, which is checked before the status code.

**6. Where is runtime polymorphism used, and why is it needed there?**
`CodeAgentError` declares `code()` and `httpStatus()` as pure virtual functions. The GUI's `respondWithError(const CodeAgentError&)` calls them through a base reference, so one function produces the right status and code for all twelve error types. `NativeDialogProvider::choose` is pure virtual so the server can use the platform dialog in production and a scripted one in tests.

**7. Why is the `CodeAgentError` constructor `protected`, and why are some classes `final`?**
The base class is abstract and must only be created as one of its concrete subclasses. Classes with no meaningful specialisation are `final`, which documents that the hierarchy ends there.

**8. Why does the exception class store its diagnostic in a `shared_ptr<const std::string>`?**
Exceptions are copied while they are thrown and caught. If copying could throw, a second exception during propagation would call `std::terminate`. A `shared_ptr` copy is `noexcept`; `static_assert(std::is_nothrow_copy_constructible_v<…>)` in the tests enforces it for every class.

**9. What is RAII, and what happens if an exception is thrown in the middle of a Gemini request?**
Resources are owned by objects whose destructors release them. The libcurl handle and header list are held in `std::unique_ptr` with custom deleters, so when `performHttpRequest` throws `TimeoutError`, stack unwinding runs the destructors and both are freed. The same pattern closes file descriptors, removes temporary files, releases proposal reservations, and unlocks mutexes.

**10. When do you use `unique_ptr` and when `shared_ptr`?**
`unique_ptr` for single ownership: the server's pimpl and the libcurl handles. `shared_ptr` where ownership is genuinely shared: the dialog provider, which tests create and pass in, and immutable exception members, where sharing makes copies cheap and non-throwing. There is no owning raw `new`/`delete`.

**11. Why does `GuiHttpServer` use the pimpl idiom?**
Its header exposes only the public operations. Threads, mutexes, proposals, secrets, and the cpp-httplib types stay in the `.cpp` file, so users of the class do not compile against cpp-httplib and the internals can change without recompiling them.

**12. How do you test without calling Gemini?**
Dependency injection: `LLMClient` takes an `HttpTransport` `std::function` that tests replace with canned responses (including structured Gemini errors and thrown `TimeoutError`). `GuiHttpServer` takes handler functions, a clock, and a dialog provider; `CLIController` takes the LLM call and the terminal streams.

**13. How do you ensure the API key never leaks?**
It is read only into a header for the request. Errors never include it; `redactSecrets` removes the configured key, Google-style keys, and key-carrying parameters and headers from diagnostics; Gemini's free-text error messages are discarded; the browser receives only `what()`. Tests feed a provider response that echoes the key and assert that it appears in neither the message nor the diagnostic.

**14. How are writes made safe?**
Content goes to a temporary file that is renamed over the target. A rename is atomic, so the file is either the old version or the new one, never a truncated mix. If writing or renaming fails, the temporary file is removed and the original is untouched. Overwriting an existing file always needs confirmation.

**15. How does the GUI detect that a file changed in another editor?**
When a file is opened, the server returns a content hash (`revision`). Saving sends it back as `baseRevision`; if the file on disk no longer hashes to it, the server throws `FileConflictError` and answers `409 FILE_CHANGED_ON_DISK` unless the user confirms overwriting.

**16. Why are there no custom templates or overloaded operators?**
No behaviour in the code is needed for several unrelated types, so a template would add complexity without reuse. The project uses standard templates instantiated with its own types instead: `std::unique_ptr` with custom deleters, `std::function`, `std::optional`. The deleters overload `operator()` because `unique_ptr` calls them as functions, and RAII types delete `operator=` to prevent double release. Arithmetic or comparison operators would have no natural meaning for these classes.

**17. Is the GUI server thread-safe?**
Requests run on cpp-httplib worker threads. Shared state is split by concern and each part has its own mutex (configuration, proposals, saves, opened documents, sessions). The proposal save keeps the proposal lock until the write finishes, so a proposal cannot be evicted while it is being saved.

**18. Why `std::function` for the GUI handlers instead of an abstract class?**
Each handler is a single operation. A `std::function` accepts a lambda, which is lighter than a class with one virtual method and lets `CLIController` wire `PromptBuilder` and `LLMClient` in place and tests pass fakes inline.

**19. How is the local server protected from other websites?**
It listens only on `127.0.0.1`. Every API call needs a random 256-bit session token, obtained once through a single-use code in the URL fragment. The server also checks `Origin` and `Host`, which blocks cross-site requests and DNS rebinding.

**20. What are the main limitations?**
Linux/WSL2 only; one C/C++ file at a time up to 512 KiB; no syntax highlighting; generated code is not compiled or run; it depends on Gemini's availability and quotas; and there is no automated real-browser test.
