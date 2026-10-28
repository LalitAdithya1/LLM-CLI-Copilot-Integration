#!/usr/bin/env bash
# Builds CodeAgent and installs the `codeagent` command so it runs from any
# directory. Installs into ~/.local by default, which needs no sudo:
#   ~/.local/bin/codeagent
#   ~/.local/share/codeagent/frontend/   (the GUI, found relative to the binary)
#
# Usage: scripts/install.sh [--prefix DIR] [--skip-tests] [--uninstall]
set -euo pipefail

prefix="${HOME}/.local"
run_tests=1
uninstall=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="${2:?--prefix needs a directory}"; shift 2 ;;
        --skip-tests) run_tests=0; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${repository}/build-release"
manifest="${build}/install_manifest.txt"

if [[ ${uninstall} -eq 1 ]]; then
    if [[ ! -f "${manifest}" ]]; then
        echo "Nothing to uninstall: ${manifest} not found." >&2
        exit 1
    fi
    # The manifest records the last install from this build folder. Refuse if
    # that install went to another prefix, so files elsewhere are never removed.
    # (CMake writes the last line without a newline, hence the || test.)
    files=()
    while IFS= read -r file || [[ -n "${file}" ]]; do
        if [[ "${file}" != "${prefix}/"* ]]; then
            echo "The last install from ${build} went elsewhere (${file})." >&2
            echo "Run: scripts/install.sh --uninstall --prefix <that prefix>" >&2
            exit 1
        fi
        files+=("${file}")
    done < "${manifest}"
    # Removes exactly the files the last install created; configuration in
    # ~/.config/codeagent is left alone.
    for file in "${files[@]}"; do rm -f -- "${file}"; done
    # Only empty folders inside share/codeagent; never the prefix itself.
    if [[ -d "${prefix}/share/codeagent" ]]; then
        find "${prefix}/share/codeagent" -depth -type d -empty -delete
    fi
    echo "Removed the files listed in ${manifest}."
    exit 0
fi

echo "==> Configuring ${build}"
cmake -S "${repository}" -B "${build}" -DCMAKE_BUILD_TYPE=Release
echo "==> Building"
cmake --build "${build}" -j "$(nproc 2>/dev/null || echo 2)"
if [[ ${run_tests} -eq 1 ]]; then
    echo "==> Running the tests"
    ctest --test-dir "${build}" --output-on-failure
fi
echo "==> Installing into ${prefix}"
cmake --install "${build}" --prefix "${prefix}"

echo
echo "Installed ${prefix}/bin/codeagent"
case ":${PATH}:" in
    *":${prefix}/bin:"*)
        echo "${prefix}/bin is on your PATH: run 'codeagent help' from any directory." ;;
    *)
        echo "${prefix}/bin is not on your PATH yet. Add it, then open a new terminal:"
        echo "    echo 'export PATH=\"${prefix}/bin:\$PATH\"' >> ~/.bashrc" ;;
esac

config="${XDG_CONFIG_HOME:-${HOME}/.config}/codeagent/.env"
if [[ -z "${GEMINI_API_KEY:-}" && ! -f "${config}" ]]; then
    echo
    echo "To use Gemini from any directory, store your key in ${config}:"
    echo "    mkdir -p \"$(dirname "${config}")\""
    echo "    printf 'GEMINI_API_KEY=%s\\n' 'your-key' > \"${config}\""
    echo "    chmod 600 \"${config}\""
fi
