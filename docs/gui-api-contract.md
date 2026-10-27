# GUI API Contract

**Project:** C++ Coding Copilot  
**Milestone:** B1 — GUI API contract  
**Status:** Proposed contract for implementation  
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

- Bind only to `127.0.0.1` or `::1`, never a public interface.
- Use JSON request bodies (`Content-Type: application/json`) and JSON responses.
- Capture the terminal's current working directory when `codeagent gui` starts. This is the immutable default output directory.
- Save-directory precedence: explicit directory in the save request, then session-selected directory, then launch directory.
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
| `POST /api/files/open-dialog` | Open a native file picker, validate the selected C/C++ file, and return its path without reading its contents. |
| `POST /api/output/select-directory` | Open a directory picker or set a validated session output directory. |
| `POST /api/save` | Save generated code or an approved proposal to the requested output location. |
| `POST /api/session/exit` | Request graceful backend shutdown. |

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

On success, return HTTP `200` with exactly the fields shown in the response example above. The returned proposal has status `PENDING`; creating it does not approve it and does not read or write files. A successful no-op refactor is still represented as a pending proposal; later review behavior is outside B7.

`editorRevision` is an opaque, non-empty client revision token. The backend retains it with the pending proposal and does not reinterpret or return it in the B7 response. `baseEditorHash` is `fnv1a64:` followed by a 16-character, lowercase hexadecimal FNV-1a 64-bit hash of the exact UTF-8 bytes of `sourceCode`. This stable non-cryptographic hash is for accidental editor-content mismatch detection, not authentication or security. A later stale-editor check should compare both the revision token and source hash.

Proposal IDs are sequential (`prop-1`, `prop-2`, and so on) within one server process. Pending proposal data is held in memory and is discarded when the server stops; it is not persisted. B7 stores proposal state for later review endpoints but does not implement retrieval, approval, rejection, deletion, or saving.

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

B8 errors use the common JSON error object from Section 2:

| Status | Error code | Meaning |
|---|---|---|
| `400` | `INVALID_JSON` | The diff request body is not valid JSON. |
| `400` | `INVALID_REQUEST` | The diff request does not match one of the documented shapes, or an approval/rejection request has a body. |
| `404` | `PROPOSAL_NOT_FOUND` | The requested proposal ID does not exist in this server session. |
| `409` | `INVALID_PROPOSAL_STATE` | The proposal is no longer pending. |
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

The selected path must be an existing directory. On success the session's `activeOutputDirectory` is updated and HTTP `200` returns:

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
| `500` | `DIALOG_FAILED` | The native dialog could not complete. |
| `503` | `DIALOG_UNAVAILABLE` | The dialog utility or graphical display is unavailable. |
| `504` | `DIALOG_TIMEOUT` | The native dialog exceeded its 30-second timeout. |

On Ubuntu/Linux, the production provider invokes `zenity` directly with fixed argument vectors (no shell); it uses the graphical `DISPLAY` or `WAYLAND_DISPLAY` environment and a 30-second timeout. Windows currently reports the native dialog as unavailable. The provider is injectable so HTTP behavior can be tested offline.

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
- Inspect `FileManager` before GUI integration. An HTTP request must not hang waiting for terminal input.

## 5. Security and Error Handling

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
- Build, automated test results, and native-dialog manual checks are reported honestly.

For assessment quality, keep HTTP handling, proposal state, AI orchestration, and file operations separately testable. Use RAII and clear ownership. Add interfaces, inheritance, templates, or operator overloads only when they solve a genuine design problem; document the actual implementation in UML rather than inventing classes for the diagram.
