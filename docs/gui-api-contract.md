# GUI API Contract

**Project:** C++ Coding Copilot  
**Milestone:** B1 — GUI API contract  
**Status:** Implemented API contract; see current verification and platform notes
**Transport:** HTTP/JSON over loopback only

## Table of Contents

1. [Purpose and Architecture](#1-purpose-and-architecture)
2. [Common Rules](#2-common-rules)
3. [Endpoints](#3-endpoints)
4. [Proposal and Save Safety](#4-proposal-and-save-safety)
5. [Security and Error Handling](#5-security-and-error-handling)
6. [Testing and Acceptance](#6-testing-and-acceptance)

## 1. Purpose and Architecture

The GUI provides a chat panel and a code editor. Users can generate code, request refactoring, review a diff, approve or reject a proposal, choose files/directories, and save code.

The browser handles presentation and editor state. The C++ backend handles API requests, AI calls, proposal state, native dialogs, and filesystem access. Reuse existing `PromptBuilder`, `LLMClient`, `ResponseParser`, and `FileManager` where appropriate.

Intended separation:

```text
Browser UI → API client → C++ HTTP routes
                           ├── Generation/refactoring → existing LLM modules
                           ├── Proposal state and diff
                           └── File operations → FileManager/filesystem
```

This is a logical design; class names and boundaries must be confirmed against the actual implementation.

## 2. Common Rules

- The backend binds to IPv4 loopback `127.0.0.1` and validates each `/api/` request's single `Host` header against `127.0.0.1:<actual backend port>` or `localhost:<actual backend port>`. Other hosts, ports, missing Host headers, and duplicate Host headers receive `400 INVALID_HOST`.
- The supported browser GUI origins are exactly the server's own origins `http://127.0.0.1:<port>` and `http://localhost:<port>` (where `codeagent gui` serves the bundled frontend) plus the development origins `http://127.0.0.1:5500` and `http://localhost:5500`. Responses to an allowed origin include `Access-Control-Allow-Origin` matching that origin; wildcard CORS is not used.
- The server handles preflight `OPTIONS` for API paths only when the Origin is a configured GUI origin, the requested method is `GET` or `POST`, and requested headers are absent or limited to `Content-Type`. Accepted preflights advertise `GET, POST, OPTIONS` and `Content-Type`.
- Any present Origin on an API request must be exactly one of those supported origins. Foreign, `null`, malformed, or duplicate Origin headers receive `403 ORIGIN_NOT_ALLOWED` before route handlers run, including state-changing routes with empty bodies.
- Requests with no Origin are accepted as non-browser local clients for CLI and automated-test compatibility. Host validation still applies. This is not client authentication: other local processes can make requests, and browser protection relies on browsers sending Origin for cross-origin requests. Do not expose the backend beyond loopback.
- Use JSON request bodies (`Content-Type: application/json`) and JSON responses.
- Capture the terminal's current working directory when `codeagent gui` starts. This is the immutable default output directory.
- Save-directory precedence: session-selected directory, then launch directory. The save request does not accept a directory path.
- The C++ backend performs filesystem reads/writes. The browser must not access arbitrary host paths directly.
- Never expose API keys, `.env` contents, or stack traces.
- Use a consistent error shape:

```json
{
  "error": {
    "code": "INVALID_INSTRUCTION",
    "message": "Instruction cannot be empty."
  }
}
```

Typical status codes: `400` invalid request, `404` missing resource, `409` state/version conflict, `413` oversized request, `422` unsupported file/path, `500` internal or filesystem error, `502` AI provider error, `503` missing configuration/unavailable dialog, `504` AI timeout.

All `/api/` errors use this JSON shape. Unknown API routes return `404 NOT_FOUND`; unsupported methods return a safe JSON error (currently `404 NOT_FOUND` when cpp-httplib cannot match a route). Unhandled API exceptions return `500 INTERNAL_ERROR`. Request bodies are limited to 1 MiB, provider response bodies to 2 MiB, and proposal retention to 128 entries per server process. Refactor capacity is reserved before provider invocation; reservations count against the same 128-entry limit and are released on every failed request path.

## 3. Endpoints

| Method and path | Purpose |
|---|---|
| `GET /api/health` | Check that the backend is responding. |
| `GET /api/config` | Return safe configuration, supported extensions, and launch/active output directories. Never return secrets. |
| `POST /api/generate` | Generate code from an instruction and language. Does not save automatically. |
| `POST /api/refactor` | Accept editor source and instruction; return a pending proposal. Does not modify files. |
| `POST /api/diff` | Return a diff for two code buffers or an existing proposal ID. |
| `POST /api/changes/{id}/approve` | Mark a pending proposal as approved. Does not save. |
| `POST /api/changes/{id}/reject` | Reject a pending proposal. It cannot subsequently be saved. |
| `POST /api/changes/{id}/mark-applied` | Record that an approved proposal was applied to the client's editor. Writes no file; frees the proposal for eviction. |
| `POST /api/files/open-dialog` | Open a native file picker, validate the selected C/C++ file, and return its path without reading its contents (the editor then calls `/api/files/read`). |
| `POST /api/files/read` | Open a user-chosen source file by path for editing; returns its content and revision. |
| `POST /api/files/write` | Save editor content back to a file opened with `/api/files/read`, detecting changes made on disk since. |
| `POST /api/output/select-directory` | Open a directory picker or set a validated session output directory. |
| `POST /api/save` | Save generated code or an approved proposal to the requested output location. |

### B4 response schemas (agreed API contract)

The following response fields and shapes are the agreed API contract for the B4 endpoints.

#### `GET /api/health`

On success, return HTTP `200` with a JSON response:

```json
{
  "status": "ok"
}
```

#### `GET /api/config`

On success, return HTTP `200` with a JSON object containing exactly these fields:

- `supportedExtensions`: an array of supported source-file extension strings.
- `launchDirectory`: the absolute path captured when the GUI session is initialized.
- `activeOutputDirectory`: the absolute path currently selected for output. Initially, it is equal to `launchDirectory`.

The current supported extension list is:

```json
[
  ".c",
  ".cc",
  ".cpp",
  ".cxx",
  ".h",
  ".hh",
  ".hpp",
  ".hxx"
]
```

Example successful response:

```json
{
  "supportedExtensions": [".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"],
  "launchDirectory": "/absolute/path/to/launch/directory",
  "activeOutputDirectory": "/absolute/path/to/launch/directory"
}
```

### B6: `POST /api/generate`

The request must use `Content-Type: application/json` and contain exactly two string fields: `instruction` and `language`. The body is limited to 1 MiB. Both values must contain non-whitespace text. The current backend has no language allow-list; it accepts any non-empty language label. Additional fields, including credential fields, are rejected. Generation returns code only and does not write files.

Request:

```json
{
  "instruction": "Write a C++ program that prints Hello World.",
  "language": "C++"
}
```

Success response (`200`):

```json
{
  "generatedCode": "#include <iostream>\\nint main() { return 0; }",
  "language": "C++"
}
```

Errors use the common JSON error object from Section 2. B6 returns:

| Status | Error code | Meaning |
|---|---|---|
| `400` | `INVALID_JSON` | The body is not valid JSON. |
| `400` | `INVALID_REQUEST` | The request is not an object with exactly the required string fields. |
| `400` | `INVALID_INSTRUCTION` | `instruction` is empty or whitespace-only. |
| `400` | `INVALID_LANGUAGE` | `language` is empty or whitespace-only. |
| `413` | `PAYLOAD_TOO_LARGE` | The request body exceeds 1 MiB. |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | The request content type is not `application/json`. |
| `502` | `GENERATION_FAILED` | Prompt generation or the provider call failed. |
| `503` | `GENERATION_UNAVAILABLE` | No generation handler is configured. |

For example, an invalid instruction returns HTTP `400`:

```json
{
  "error": {
    "code": "INVALID_INSTRUCTION",
    "message": "Instruction cannot be empty."
  }
}
```

### Example: create a refactoring proposal

Request:

```json
{
  "sourceCode": "void greet() {}",
  "instruction": "Improve readability.",
  "language": "C++",
  "editorRevision": "revision-1"
}
```

Success response (`200`):

```json
{
  "proposalId": "prop_example_001",
  "status": "PENDING",
  "originalCode": "void greet() {}",
  "proposedCode": "void greet() noexcept {}",
  "baseEditorHash": "illustrative-hash"
}
```

Examples are illustrative, not actual model output.

### B7: `POST /api/refactor`

The request must use `Content-Type: application/json` and contain exactly four string fields: `sourceCode`, `instruction`, `language`, and `editorRevision`. Each field must contain non-whitespace text. Language labels are not restricted to an allow-list. Additional fields are rejected. The request body limit is 1 MiB.

On success, return HTTP `200` with the documented fields. The returned proposal has status `PENDING`; creating it does not approve it and does not read or write files. A no-op refactor is still represented as a pending proposal. The diff, decision, and save routes below operate on this proposal.

`editorRevision` is an opaque, non-empty client revision token. The backend retains it with the pending proposal and does not reinterpret or return it in the B7 response. `baseEditorHash` is `fnv1a64:` followed by a 16-character, lowercase hexadecimal FNV-1a 64-bit hash of the exact UTF-8 bytes of `sourceCode`. This stable non-cryptographic hash is for accidental editor-content mismatch detection, not authentication or security. A later stale-editor check should compare both the revision token and source hash.

Proposal IDs are sequential (`prop-1`, `prop-2`, and so on) within one server process. Proposals are held in memory and discarded when the server stops; they are not persisted. At most 128 proposals are retained, counting in-flight creation reservations. A refactor request reserves a free slot or pins the oldest unreserved `REJECTED` or `APPLIED` proposal for replacement before invoking the provider. If no slot or terminal proposal is available, the server returns `503 PROPOSAL_CAPACITY` without calling the provider. Pending, approved, saving, and reserved proposals are never evicted. Response serialization and proposal-node allocation are staged before replacement; the old terminal proposal remains stored if provider or preparation fails. Concurrent reservations cannot claim the same terminal entry or exceed the cap. Failed requests do not advance the proposal ID counter.

Errors use the common JSON error object from Section 2:

| Status | Error code | Meaning |
|---|---|---|
| `400` | `INVALID_JSON` | The body is not valid JSON. |
| `400` | `INVALID_REQUEST` | The body is not an object with exactly the required string fields. |
| `400` | `INVALID_SOURCE_CODE` | `sourceCode` is empty or whitespace-only. |
| `400` | `INVALID_INSTRUCTION` | `instruction` is empty or whitespace-only. |
| `400` | `INVALID_LANGUAGE` | `language` is empty or whitespace-only. |
| `400` | `INVALID_EDITOR_REVISION` | `editorRevision` is empty or whitespace-only. |
| `413` | `PAYLOAD_TOO_LARGE` | The request body exceeds 1 MiB. |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | The request content type is not `application/json`. |
| `502` | `REFACTOR_FAILED` | The handler failed, threw, returned empty output, or the result could not be serialized. No proposal is created. |
| `503` | `REFACTOR_UNAVAILABLE` | No refactoring handler is configured. |
| `503` | `PROPOSAL_CAPACITY` | No unreserved free slot or terminal proposal is available; the provider is not invoked. |

The production handler builds the prompt with `PromptBuilder` and calls `LLMClient::generateCompleteContent()`, which validates the provider response through `ResponseParser`. Blocked, incomplete, malformed, empty, or failed provider responses therefore produce `REFACTOR_FAILED`; exception details and provider response text are not returned to the HTTP client.

### B8: Diff and proposal review

#### `POST /api/diff`

The request must use `Content-Type: application/json`. It accepts exactly one of these shapes:

```json
{"proposalId":"prop-1"}
```

or

```json
{"originalCode":"void greet() {}","proposedCode":"void greet() noexcept {}"}
```

Direct code buffers may be empty. Proposal-based diff uses the stored original and proposed buffers. The response is HTTP `200` with `{"diff":"..."}`. Reading a proposal's diff never changes its status. Requests remain subject to the 1 MiB body limit.

#### `POST /api/changes/{id}/approve` and `POST /api/changes/{id}/reject`

The proposal ID is supplied in the path and the request body must be empty. Success returns HTTP `200` with `{"proposalId":"prop-1","status":"APPROVED"}` or `{"proposalId":"prop-1","status":"REJECTED"}`. Only a `PENDING` proposal may transition. Repeating a transition or attempting the opposite transition returns HTTP `409`; an unknown ID returns HTTP `404`. These operations only change proposal state; neither operation saves or applies code.

The backend retains `editorRevision` and `baseEditorHash` unchanged in proposal state. B8 does not perform a stale-editor check: the contract requires rejecting stale application, while B8 approval does not apply code and has no current-revision field to compare. The stale check belongs on a future apply/save operation.

The current browser UI additionally compares the editor text with the proposal's original source and disables approval/save if it has changed. This is a client-side guard only; the backend cannot verify freshness across clients because decision/save requests do not include a current revision or content hash.

#### `POST /api/changes/{id}/mark-applied`

The request body must be empty. Only an `APPROVED` proposal may transition; success returns HTTP `200` with `{"proposalId":"prop-1","status":"APPLIED"}`. A pending, rejected, or already applied proposal returns `409 INVALID_PROPOSAL_STATE` and is not changed; an unknown ID returns `404`. The operation writes no file. It moves the proposal to the same terminal `APPLIED` state as a proposal saved through `/api/save`, so it can no longer be saved and becomes eligible for eviction at capacity.

The browser UI's *Accept* first approves the proposal, then replaces the editor contents with the proposed code, and only after that sends `mark-applied`. If `mark-applied` fails, the editor keeps the accepted code and the proposal simply stays `APPROVED` (protected from eviction) until the server restarts.

B8 errors use the common JSON error object from Section 2:

| Status | Error code | Meaning |
|---|---|---|
| `400` | `INVALID_JSON` | The diff request body is not valid JSON. |
| `400` | `INVALID_REQUEST` | The diff request does not match one of the documented shapes, or an approve/reject/mark-applied request has a body. |
| `404` | `PROPOSAL_NOT_FOUND` | The requested proposal ID does not exist in this server session. |
| `409` | `INVALID_PROPOSAL_STATE` | The proposal is not pending (approve/reject) or not approved (mark-applied). |
| `413` | `PAYLOAD_TOO_LARGE` | A B8 request body exceeds 1 MiB. |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | The diff request content type is not `application/json`. |
| `500` | `DIFF_FAILED` | The diff could not be generated. |
| `500` | `STATE_UPDATE_FAILED` | A transition response could not be prepared. |

### B9: Native file and output-directory selection

Both endpoints require `Content-Type: application/json` and are limited by the common 1 MiB request-body limit. They only inspect path metadata; selecting a source file never reads its contents, and neither endpoint writes files.

#### `POST /api/files/open-dialog`

The request body must be the empty JSON object `{}`. It opens the native source-file picker. A selected path must exist, be a regular file, and have one of the extensions returned by `GET /api/config` (`.c`, `.cc`, `.cpp`, `.cxx`, `.h`, `.hh`, `.hpp`, `.hxx`). Success returns HTTP `200`:

```json
{"status":"SELECTED","selectedPath":"/absolute/path/to/source.cpp"}
```

Cancellation returns HTTP `200` with `{"status":"CANCELLED"}`. No file content is included in any response.

#### `POST /api/output/select-directory`

The request may be `{}` to open the native directory picker, or exactly one string `directoryPath` for manual path selection:

```json
{"directoryPath":"/absolute/path/to/output"}
```

The selected path must be an existing, writable directory. Under WSL, a Windows path such as `D:\out` or `\\wsl.localhost\Ubuntu\home\me` is first translated to `/mnt/d/out` or `/home/me`. On success the session's `activeOutputDirectory` is updated and HTTP `200` returns:

```json
{"status":"SELECTED","activeOutputDirectory":"/absolute/path/to/output"}
```

Cancellation returns HTTP `200` with `{"status":"CANCELLED"}` and leaves the active directory unchanged. Invalid paths also leave it unchanged.

B9 errors use the common JSON error object from Section 2:

| Status | Error code | Meaning |
|---|---|---|
| `400` | `INVALID_JSON` | The request body is not valid JSON. |
| `400` | `INVALID_REQUEST` | The body does not match the endpoint's documented shape. |
| `400` | `INVALID_DIRECTORY_PATH` | `directoryPath` is empty or whitespace-only. |
| `413` | `PAYLOAD_TOO_LARGE` | The request body exceeds 1 MiB. |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | The content type is not `application/json`. |
| `422` | `INVALID_SOURCE_FILE` | The selected path is not an existing regular source file with a supported extension. |
| `422` | `INVALID_OUTPUT_DIRECTORY` | The selected path is not an existing directory. |
| `422` | `OUTPUT_DIRECTORY_NOT_WRITABLE` | The selected directory exists but cannot be written. |
| `500` | `DIALOG_FAILED` | The native dialog could not complete. |
| `503` | `DIALOG_UNAVAILABLE` | The dialog utility or graphical display is unavailable. |
| `504` | `DIALOG_TIMEOUT` | The native dialog exceeded its 2-minute timeout. |

The POSIX provider invokes `zenity` with fixed argument vectors through `posix_spawn` (no shell) when `DISPLAY` or `WAYLAND_DISPLAY` is set. Under WSL, if zenity is missing, it starts `powershell.exe` through WSL interop with a fixed Windows Forms script and translates the returned Windows path. Returned path data is capped and dialogs time out after 2 minutes. A native Windows build reports dialogs unavailable. On macOS, this implementation requires a separately installed `zenity`. Automated tests use an injected provider and do not exercise a real desktop dialog.

### B10: Editing files in place

#### `POST /api/files/read`

Request: exactly `{"path": "<absolute or Windows path>"}`. The path is translated under WSL, then must name an existing regular file whose canonical (symlink-resolved) path has a supported extension, is at most 512 KiB, and is valid UTF-8. Success returns HTTP `200`:

```json
{"path":"/canonical/path/insertion_sort.cpp","content":"...","revision":"fnv1a64:0123456789abcdef"}
```

The canonical path is remembered for this server session as an in-place save target. Errors: `400 INVALID_REQUEST`/`INVALID_JSON`, `413 FILE_TOO_LARGE`, `403 READ_FAILED`, `422 INVALID_SOURCE_FILE`, `422 UNSUPPORTED_ENCODING`.

#### `POST /api/files/write`

Request: exactly `{"path": string, "content": string, "baseRevision": string, "overwriteConfirmed": boolean}`. `path` must be a canonical path previously returned by `/api/files/read` in this session; any other path is refused with `403 FILE_NOT_OPENED`. If the file on disk no longer matches `baseRevision` (it was edited elsewhere or deleted) and `overwriteConfirmed` is false, the response is `409 FILE_CHANGED_ON_DISK` and nothing is written. The write is atomic, preserves the file's permission bits, and refuses symlinks and non-regular files (`422 INVALID_TARGET`). Permission failures return `403 PERMISSION_DENIED`. Success returns HTTP `200`:

```json
{"savedPath":"/canonical/path/insertion_sort.cpp","status":"SAVED","revision":"fnv1a64:fedcba9876543210"}
```

### Example: save generated code

Request:

```json
{
  "fileName": "Example.cpp",
  "content": "#include <iostream>\\nint main() { return 0; }",
  "overwriteConfirmed": false
}
```

Success response (`200`):

```json
{
  "savedPath": "/example/project/Example.cpp",
  "status": "SAVED"
}
```

For a proposal save, the request uses `proposalId` and `fileName`, with optional boolean `overwriteConfirmed`; proposal content comes only from server state. Saves use the session-selected directory, then launch directory. No per-request directory is accepted.

Both forms require JSON and are limited to 1 MiB. Generated-code saves contain exactly `fileName`, `content`, and boolean `overwriteConfirmed`. Proposal saves contain `proposalId`, `fileName`, and optional `overwriteConfirmed`. Filenames must be single supported C/C++ source filenames; paths, separators, traversal, absolute names, symbolic links, and non-regular targets are rejected. The output directory must already exist. Success is HTTP `200` with `{"savedPath":"/absolute/path/name.cpp","status":"SAVED"}`. Errors use the common JSON shape: `400 INVALID_JSON`, `INVALID_REQUEST`, or `INVALID_FILENAME`; `404 PROPOSAL_NOT_FOUND`; `409 INVALID_PROPOSAL_STATE` or `OVERWRITE_CONFIRMATION_REQUIRED`; `413 PAYLOAD_TOO_LARGE`; `415 UNSUPPORTED_MEDIA_TYPE`; `422 INVALID_OUTPUT_DIRECTORY` or `INVALID_TARGET`; `500 SAVE_FAILED`.

The browser UI currently saves approved proposals only. It first sends `overwriteConfirmed: false`; if the backend reports `OVERWRITE_CONFIRMATION_REQUIRED`, it asks the user and retries with `true` only after confirmation. Generated-code saving is supported by the backend route but is not offered by the current UI.

## 4. Proposal and Save Safety

Proposal states:

```text
PENDING ──approve──> APPROVED ──successful save──> APPLIED
   └────reject────> REJECTED
```

- Viewing a diff never approves a proposal.
- Refactoring never writes to disk automatically.
- Only `APPROVED` proposals may be saved.
- Rejected or already-applied proposals cannot be saved again.
- Unknown proposal IDs return `404`; invalid state transitions return `409`.
- Save requests do not carry the current editor revision/hash, so stale-editor validation cannot be performed. Proposal state preserves its original `editorRevision` and `baseEditorHash`; the backend does not claim freshness validation until the request contract defines current revision/hash fields.
- If a proposal is based on an existing file, track its on-disk baseline separately from the editor-buffer version. Do not compare these as if they were the same hash.
- Require explicit confirmation before overwriting an existing file.
- Use a unique temporary file per write and safe platform-appropriate replacement. On failure, clean up the temporary file where possible, preserve the existing target where supported, and leave the proposal approved for retry.
- A successful proposal save transitions `APPROVED` to `APPLIED`; a failed write leaves it approved and retryable.
- The UI displays `APPLIED` and the saved path only after a valid save success response.
- At the 128-proposal limit, only the oldest rejected or applied proposal is eligible for eviction. The proposal mutex is held across state changes and save I/O, so creation cannot evict a proposal while its save is in progress. If all entries are pending or approved, creation returns `503 PROPOSAL_CAPACITY`.
- Inspect `FileManager` before GUI integration. An HTTP request must not hang waiting for terminal input.

## 5. Security and Error Handling

`./build/codeagent gui` serves the `frontend` directory and the API from the same loopback origin (default `http://127.0.0.1:8080`, override with `CODEAGENT_GUI_PORT`) and opens it in the default browser. For frontend-only development, the directory can still be served separately at `http://127.0.0.1:5500`; the page then calls the API on port 8080. Other frontend origins require an intentional backend policy change; they are not implicitly trusted.

Origin checks are server-side request checks; CORS response headers alone do not authorize state changes. Missing Origin remains permitted for non-browser loopback callers as described above.

- Validate request fields, file extensions, filenames, and paths.
- Reject path traversal outside the permitted output directory. Do not rely on string normalization alone where symlinks can affect the result.
- Restrict allowed browser origins and validate the expected host. CORS alone is not sufficient protection for a loopback server.
- Set request-size limits and an explicit upstream AI timeout.
- Return safe, consistent error messages without secrets or stack traces.
- Handle native-dialog cancellation without changing the active directory or crashing.
- Verify actual platform support for native dialogs, especially in WSL/headless environments.
- Protect shared proposal state against concurrent requests. A proposal must not be saved twice due to a race.
- Shutdown should be coordinated by the server lifecycle owner, not performed unsafely inside a request callback.

## 6. Testing and Acceptance

Before calling the GUI backend complete, verify:

- Existing CLI `generate`, `refactor`, and `help` still work.
- Health/config endpoints return valid JSON and no secrets.
- Invalid requests and AI failures return the documented errors.
- Refactoring does not write files until approval and save.
- Diff, approve, reject, stale proposal, and duplicate-save behavior are tested.
- Default and selected output directories work as specified.
- Existing-file conflicts and failed writes are handled safely.
- Automated tests do not require live AI calls; use a test seam or fake where appropriate.
- The frontend uses the same endpoint paths and JSON shapes.
- Frontend checks run with `node frontend/tests/api.test.mjs`, `node frontend/tests/proposalWorkflow.test.mjs`, `node frontend/tests/appDomIntegration.test.mjs`, and `node --check` on the JavaScript modules. The DOM integration suite uses a small dependency-free test DOM and executes the actual app module; it is not a substitute for a browser smoke test.
- Build, automated test results, and native-dialog manual checks are reported honestly.

For assessment quality, keep HTTP handling, proposal state, AI orchestration, and file operations separately testable. Use RAII and clear ownership. Add interfaces, inheritance, templates, or operator overloads only when they solve a genuine design problem; document the actual implementation in UML rather than inventing classes for the diagram.

The current browser editor is a plain text area, not Monaco. The source-file dialog returns a validated path but does not read file contents; users must paste source into the editor to refactor it. Files mode, settings, and copy controls remain disabled placeholders. There is no `/api/session/exit` route; stop the backend from its owning terminal with Ctrl+C. No browser automation is configured in this repository.

Manual browser verification: build and start `./build/codeagent gui` and use the page it opens at `http://127.0.0.1:8080/` (not `file://`). Confirm health/config, generate a response with a configured provider, enter source text and create a refactor proposal, request and inspect its diff, approve it, enter a supported filename, and save. Repeat with a second proposal and reject it; verify it cannot be saved. To verify overwrite handling, save an approved proposal to an existing file and confirm the prompt before replacement. Editing the source after proposal creation should disable approval and save until the original text is restored. The provider-backed steps can incur provider usage and require valid configuration.

Manual native-dialog verification on Linux requires a graphical session and `zenity`: use both source and output-directory buttons, confirm selections return paths, cancel each dialog and confirm the active output directory remains unchanged, and test an unsupported file selection. The Windows implementation reports unavailable. macOS has no native dialog implementation and depends on the POSIX `zenity` path if installed. Automated dialog tests use a fake provider; they verify cancellation, failure, timeout, and path validation without opening a desktop dialog.
