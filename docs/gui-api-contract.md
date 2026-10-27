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
| `POST /api/files/open-dialog` | Open a native file picker, validate the selected C/C++ file, and return its path and contents. |
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

### Example: generate code

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

For a proposal save, the request uses `proposalId` and `fileName`; the proposal must already be approved. The backend returns the final resolved path after a successful write.

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
- Track the original editor version. If the editor changes before applying a proposal, reject stale application rather than silently replacing newer work.
- If a proposal is based on an existing file, track its on-disk baseline separately from the editor-buffer version. Do not compare these as if they were the same hash.
- Require explicit confirmation before overwriting an existing file.
- Use a unique temporary file per write and safe platform-appropriate replacement. On failure, clean up the temporary file where possible, preserve the existing target where supported, and leave the proposal approved for retry.
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
