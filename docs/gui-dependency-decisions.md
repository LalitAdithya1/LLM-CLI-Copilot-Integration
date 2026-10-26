# GUI Dependency Decisions

**Milestone:** B2 — GUI dependency choices  
**Status:** Recommendations for implementation; platform checks remain open

## Verified repository facts

- The project uses **C++17** with CMake (minimum version 3.15) and currently requires `libcurl` and `nlohmann/json`. GoogleTest is fetched by CMake for tests.
- The application is organized as a CLI under `src/` and `include/`, with tests under `tests/`. There is no GUI frontend, HTTP server, or native-dialog code yet.
- `LLMClient` already uses libcurl as an HTTP client and nlohmann/json for JSON. `FileManager` uses `std::filesystem`. These are reusable existing dependencies, not an HTTP server or native-dialog solution.
- The B1 contract specifies HTTP/JSON on loopback, a browser UI, API routes for file and directory dialogs, and safe static-file behavior and origin handling. It also calls for verifying native-dialog support in WSL and headless environments.

## Recommendations

| Need | Recommendation | Main trade-off |
|---|---|---|
| C++ HTTP server | **cpp-httplib** for the first GUI backend. It is a small, cross-platform C++11+ library with a straightforward route API, making it suitable for C++17 and a local, low-traffic server. | Simple integration, but it adds a dependency to vendor or fetch. It uses blocking HTTP/1.1 I/O, so its thread pool, request limits, and body-size limits need deliberate configuration. |
| Alternative server | **Boost.Beast** if future needs justify asynchronous networking or the project already adopts Boost. | Portable and capable, but lower-level than a turn-key server framework. It requires more lifecycle, parsing, and routing code for this small API. |
| Frontend serving | During development, run the frontend development server and proxy `/api` requests to the C++ loopback server. For normal use, serve the built static frontend bundle from the C++ server so the UI and API share one origin. Resolve the installed asset directory explicitly, with a documented development override. Never assume the process working directory is the asset root. | A development server makes iteration easier but runs as a separate process. Serving the production bundle avoids a second runtime service, but packaging and asset-path resolution require implementation and testing. No frontend tool or asset tree exists in the repository yet. |
| Ubuntu file/directory picker | **Invoke `zenity` as a child process** for file and directory selection. Check executable availability, exit status, cancellation, and returned paths. This avoids adding GTK/Qt development dependencies to the C++ build. | Adds a runtime executable and a GNOME/GTK desktop expectation. It may be absent on minimal desktops. Use argument-safe process creation; never concatenate user input into a shell command. |
| Other dialog options | **tinyfiledialogs** provides a small C/C++ API with Linux backends but adds vendored code and backend/runtime assumptions. **GTK or Qt dialogs** provide more integrated native options but require substantially larger toolkit dependencies. | Consider these alternatives if zenity proves unreliable on supported Ubuntu desktops or the application grows into a native desktop shell. |

## WSL and platform limitations

WSL does not guarantee a graphical display, a working GUI bridge, or an installed `zenity`; availability depends on the WSL version and desktop configuration.

Treat native dialogs as optional at runtime. If a dialog cannot be launched, the backend should return the API contract's appropriate error response, and the GUI must offer a **manual-path fallback**. The user must be able to enter a source-file path or output-directory path without using a native dialog.

Verify both WSLg and headless behavior on the actual target setup before claiming support. A future Windows-native dialog would require a separate implementation or a cross-platform GUI toolkit; the Ubuntu `zenity` approach does not provide Windows-native dialogs.

## Decision and validation still needed

Proceed with these recommendations:

- **HTTP server:** cpp-httplib, with the existing nlohmann/json library for JSON.
- **Frontend:** development-server proxy during development; same-origin bundled static files for normal use.
- **Native dialogs:** zenity on Ubuntu, with a manual-path fallback.
- **LLM requests:** retain libcurl for outbound API calls.
- **Alternatives:** do not add Boost, GTK, or Qt solely for this milestone.

Before adding cpp-httplib, inspect the existing CMake dependency conventions and select a suitable acquisition method and pinned version. Use the project's existing conventions where practical. Do not leave the dependency version unpinned or rely on an uncontrolled latest-version download.

Before implementation is considered settled:

- Prototype cpp-httplib with appropriate request-body limits, shutdown behavior, and a restricted static-file root.
- Verify asset-path resolution from an installed launch location.
- Test zenity cancellation and missing-executable behavior.
- Test on an Ubuntu desktop, WSLg, and headless WSL where applicable.
- Confirm that manual path entry works when native dialogs are unavailable.

These are recommendations and outstanding validation tasks, not claims of verified runtime behavior.

## References

- [cpp-httplib project](https://github.com/yhirose/cpp-httplib)
- [Boost.Beast overview](https://www.boost.org/library/latest/beast/)
- [Boost.Beast design FAQ](https://live.boost.org/doc/libs/master/libs/beast/doc/html/beast/design_choices/faq.html)
- [GNOME file-dialog documentation](https://developer.gnome.org/documentation/tutorials/beginners/components/file_dialog.html)
- [tinyfiledialogs project](https://github.com/polyfem/tinyfiledialogs)
- [Zenity file-selection options](https://help.gnome.org/users/zenity/stable/file-selection.html)
