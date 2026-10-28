# GUI Dependency Decisions

**Milestone:** B2 — GUI dependency choices  
**Status:** Recommendations for implementation; platform checks remain open

## Verified repository facts

- The project uses **C++17** with CMake (minimum version 3.15), and requires `libcurl` and `nlohmann/json`. GoogleTest and cpp-httplib are fetched by CMake at pinned versions.
- The application has a C++ CLI and a loopback-only HTTP server under `src/` and `include/`. Frontend files exist separately under `frontend/`.
- `LLMClient` uses libcurl and nlohmann/json; `FileManager` uses `std::filesystem`. `GuiHttpServer` reuses the server-side API and proposal state while the existing CLI remains its own entry point.
- The API contract defines file and directory selection routes and requires safe errors, path validation, and graceful cancellation.

## Recommendations

| Need | Recommendation | Main trade-off |
|---|---|---|
| C++ HTTP server | **cpp-httplib** for the first GUI backend. It is a small, cross-platform C++11+ library with a straightforward route API, making it suitable for C++17 and a local, low-traffic server. | Simple integration, but it adds a dependency to vendor or fetch. It uses blocking HTTP/1.1 I/O, so its thread pool, request limits, and body-size limits need deliberate configuration. |
| Alternative server | **Boost.Beast** if future needs justify asynchronous networking or the project already adopts Boost. | Portable and capable, but lower-level than a turn-key server framework. It requires more lifecycle, parsing, and routing code for this small API. |
| Frontend serving | During development, run the frontend development server and proxy `/api` requests to the C++ loopback server. For normal use, serve the built static frontend bundle from the C++ server so the UI and API share one origin. Resolve the installed asset directory explicitly, with a documented development override. Never assume the process working directory is the asset root. | A development server makes iteration easier but runs as a separate process. Serving the production bundle avoids a second runtime service, but packaging and asset-path resolution require implementation and testing. No frontend tool or asset tree exists in the repository yet. |
| Ubuntu file/directory picker | **Invoke `zenity` as a child process** for file and directory selection. Check executable availability, exit status, cancellation, and returned paths. This avoids adding GTK/Qt development dependencies to the C++ build. | Adds a runtime executable and a GNOME/GTK desktop expectation. It may be absent on minimal desktops. Use argument-safe process creation; never concatenate user input into a shell command. |
| Other dialog options | **tinyfiledialogs** provides a small C/C++ API with Linux backends but adds vendored code and backend/runtime assumptions. **GTK or Qt dialogs** provide more integrated native options but require substantially larger toolkit dependencies. | Consider these alternatives if zenity proves unreliable on supported Ubuntu desktops or the application grows into a native desktop shell. |

## B9 implementation and platform limitations

`include/NativeDialog.hpp` defines an injectable `NativeDialogProvider`; `src/NativeDialog.cpp` provides the production implementation. On Linux/Ubuntu it invokes `zenity` with fixed `exec` arguments, without shell command construction. It checks for `DISPLAY` or `WAYLAND_DISPLAY`, drains the child output, enforces a 30-second timeout, and maps cancellation, missing utility, and process failure to distinct results. It adds no CMake package or linked GUI toolkit; `zenity` is an optional runtime executable.

The file-picker route returns the validated absolute path only and does not read file contents. The output-directory route accepts either a native dialog (`{}`) or a validated `directoryPath`, so manual directory selection remains available without a graphical environment. The current contract does not define a manual source-file path request, so source selection requires the native dialog. The active output directory is changed only after validation.

WSL does not guarantee a graphical display, a working GUI bridge, or an installed `zenity`; availability depends on the WSL version and desktop configuration.

Treat native dialogs as optional at runtime. The directory endpoint supports manual path selection when native dialogs are unavailable. Manual source-file path selection remains an API-contract gap and is not implemented by B9.

Verify both WSLg and headless behavior on the actual target setup before claiming support. A future Windows-native dialog would require a separate implementation or a cross-platform GUI toolkit; the Ubuntu `zenity` approach does not provide Windows-native dialogs.

## Decision and validation still needed

Implemented dependency choices:

- **HTTP server:** cpp-httplib, with the existing nlohmann/json library for JSON.
- **Frontend:** development-server proxy during development; same-origin bundled static files for normal use.
- **Native dialogs:** optional `zenity` runtime on Ubuntu/Linux; no GTK/Qt build dependency. Manual output-directory paths are accepted by the API.
- **LLM requests:** retain libcurl for outbound API calls.
- **Alternatives:** do not add Boost, GTK, or Qt solely for this milestone.

Before adding cpp-httplib, inspect the existing CMake dependency conventions and select a suitable acquisition method and pinned version. Use the project's existing conventions where practical. Do not leave the dependency version unpinned or rely on an uncontrolled latest-version download.

Remaining platform validation:

- Prototype cpp-httplib with appropriate request-body limits, shutdown behavior, and a restricted static-file root.
- Verify asset-path resolution from an installed launch location.
- Exercise real zenity cancellation, missing-executable, timeout, and path output on supported Ubuntu desktops.
- Test on an Ubuntu desktop, WSLg, and headless WSL where applicable.
- Decide whether the contract should add manual source-file path selection.

These are recommendations and outstanding validation tasks, not claims of verified runtime behavior.

## References

- [cpp-httplib project](https://github.com/yhirose/cpp-httplib)
- [Boost.Beast overview](https://www.boost.org/library/latest/beast/)
- [Boost.Beast design FAQ](https://live.boost.org/doc/libs/master/libs/beast/doc/html/beast/design_choices/faq.html)
- [GNOME file-dialog documentation](https://developer.gnome.org/documentation/tutorials/beginners/components/file_dialog.html)
- [tinyfiledialogs project](https://github.com/polyfem/tinyfiledialogs)
- [Zenity file-selection options](https://help.gnome.org/users/zenity/stable/file-selection.html)
