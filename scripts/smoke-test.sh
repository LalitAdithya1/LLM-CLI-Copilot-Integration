#!/usr/bin/env bash
# End-to-end check of an installed `codeagent`, run the way a user would: in a
# clean environment (like a new terminal) and from a folder outside the
# repository. Your real API key is never used: HOME points at an empty
# temporary folder, and the Gemini check sends a deliberately invalid key, so
# Gemini must answer "Authentication failed".
#
# Usage: scripts/smoke-test.sh [path/to/codeagent]   (default: codeagent on PATH)
set -uo pipefail

binary="${1:-$(command -v codeagent || true)}"
if [[ -z "${binary}" || ! -x "${binary}" ]]; then
    echo "codeagent was not found. Install it first: scripts/install.sh" >&2
    exit 2
fi
binary="$(cd "$(dirname "${binary}")" && pwd)/$(basename "${binary}")"

work="$(mktemp -d)"
gui_pid=""
cleanup() {
    [[ -n "${gui_pid}" ]] && kill "${gui_pid}" 2>/dev/null
    rm -rf "${work}"
}
trap cleanup EXIT
mkdir -p "${work}/home" "${work}/project"
failures=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; failures=$((failures + 1)); }

# Runs a command in a clean environment and a fresh, non-interactive shell in
# the project folder. Only the folder holding codeagent and the system folders
# are on PATH. Leading NAME=value arguments are added to the environment:
#   in_project CODEAGENT_GUI_PORT=8099 codeagent gui
in_project() {
    local -a variables=()
    while [[ $# -gt 0 && "$1" == [A-Z_]*=* ]]; do variables+=("$1"); shift; done
    env -i HOME="${work}/home" PATH="$(dirname "${binary}"):/usr/bin:/bin" \
        CODEAGENT_NO_BROWSER=1 ${variables[@]+"${variables[@]}"} \
        bash --noprofile --norc -c 'cd "$0" && exec "$@"' "${work}/project" "$@"
}

# Safety check before anything can reach Gemini: commands must run in the
# empty project folder, where no .env with a real key can be found.
if [[ "$(in_project pwd)" != "${work}/project" ]]; then
    echo "Refusing to continue: commands do not run in ${work}/project." >&2
    exit 2
fi

# 1. The command resolves from an unrelated folder.
output="$(in_project codeagent help 2>&1)"
[[ $? -eq 0 && "${output}" == *"Usage:"* ]] \
    && pass "codeagent help runs from ${work}/project" \
    || fail "codeagent help: ${output}"

# 2. A missing key is a configuration error that lists the checked files.
output="$(in_project codeagent generate "write hello world" </dev/null 2>&1)"
status=$?
[[ ${status} -eq 1 && "${output}" == *"Configuration error: GEMINI_API_KEY is not configured"* &&
   "${output}" == *"${work}/home/.config/codeagent/.env"* ]] \
    && pass "missing GEMINI_API_KEY is reported with the files checked" \
    || fail "missing key (exit ${status}): ${output}"

# 3. The per-user configuration file is found from any folder, and a
#    rejected key is reported as an authentication failure.
mkdir -p "${work}/home/.config/codeagent"
printf 'GEMINI_API_KEY=invalid-key-for-codeagent-smoke-test\n' \
    > "${work}/home/.config/codeagent/.env"
chmod 600 "${work}/home/.config/codeagent/.env"
output="$(in_project codeagent generate "write hello world" </dev/null 2>&1)"
status=$?
if [[ ${status} -eq 1 && "${output}" == *"Authentication failed"* ]]; then
    pass "~/.config/codeagent/.env is used; Gemini rejects the invalid key cleanly"
elif [[ ${status} -eq 1 && ( "${output}" == *"Network error"* || "${output}" == *"timed out"* ) ]]; then
    echo "SKIP  Gemini is unreachable from here; the network error was reported cleanly"
else
    fail "invalid key (exit ${status}): ${output}"
fi
[[ "${output}" != *"invalid-key-for-codeagent-smoke-test"* ]] \
    && pass "the API key never appears in the output" \
    || fail "the API key was printed"
[[ "${output}" != *"Do you want to save"* ]] \
    && pass "no save prompt after a failed generation" \
    || fail "save prompt shown after a failed generation"

# 4. The GUI serves its installed frontend and API from the project folder.
port=""
for attempt in 1 2 3 4 5; do
    candidate=$((20000 + RANDOM % 20000))
    in_project CODEAGENT_GUI_PORT="${candidate}" codeagent gui \
        >"${work}/gui.log" 2>&1 &
    gui_pid=$!
    for _ in $(seq 50); do
        grep -q "running at" "${work}/gui.log" 2>/dev/null && break
        kill -0 "${gui_pid}" 2>/dev/null || break
        sleep 0.1
    done
    if grep -q "running at" "${work}/gui.log"; then port="${candidate}"; break; fi
    kill "${gui_pid}" 2>/dev/null; gui_pid=""
done

if [[ -z "${port}" ]]; then
    fail "codeagent gui did not start: $(cat "${work}/gui.log")"
else
    base="http://127.0.0.1:${port}"
    install_root="$(dirname "$(dirname "${binary}")")"
    grep -q "Serving frontend from ${install_root}/share/codeagent/frontend" "${work}/gui.log" \
        && pass "GUI uses the installed frontend next to the binary" \
        || fail "frontend not taken from the install: $(grep 'Serving frontend' "${work}/gui.log")"
    page="$(curl -fsS "${base}/")"
    [[ "${page}" == *"<title>CodeAgent"* ]] \
        && pass "GUI page is served from the installed frontend" \
        || fail "GUI page not served"
    css="$(curl -fsS "${base}/css/main.css")"
    [[ "${css}" == *"--palette-beige"* ]] \
        && pass "theme stylesheet is served" \
        || fail "stylesheet not served"

    code="$(grep -o '#bootstrap=[0-9a-f]*' "${work}/gui.log" | head -1 | cut -d= -f2)"
    token="$(curl -fsS -H "Origin: ${base}" -H "Content-Type: application/json" \
        -d "{\"bootstrap\":\"${code}\"}" "${base}/api/session" |
        grep -o '"token":"[0-9a-f]*"' | cut -d'"' -f4)"
    if [[ -z "${token}" ]]; then
        fail "could not start an API session with the printed one-time link"
    else
        pass "one-time link starts an API session"
        auth=(-H "X-CodeAgent-Token: ${token}" -H "Origin: ${base}")
        config="$(curl -fsS "${auth[@]}" "${base}/api/config")"
        [[ "${config}" == *"\"launchDirectory\":\"${work}/project\""* ]] \
            && pass "GUI uses the folder it was started from" \
            || fail "unexpected config: ${config}"
        generated="$(curl -sS "${auth[@]}" -H "Content-Type: application/json" \
            -d '{"instruction":"write hello world","language":"C++"}' "${base}/api/generate")"
        if [[ "${generated}" == *'"code":"PROVIDER_AUTH_FAILED"'* ]]; then
            pass "GUI reports Gemini's rejection as PROVIDER_AUTH_FAILED"
        elif [[ "${generated}" == *'"code":"PROVIDER_UNREACHABLE"'* || "${generated}" == *'"code":"PROVIDER_TIMEOUT"'* ]]; then
            echo "SKIP  Gemini is unreachable from here; the GUI reported it cleanly"
        else
            fail "unexpected generate response: ${generated}"
        fi
        [[ "${generated}" != *"invalid-key-for-codeagent-smoke-test"* ]] \
            && pass "the API key never reaches the browser" \
            || fail "the API key reached the browser"
    fi
fi

echo
if [[ ${failures} -eq 0 ]]; then
    echo "All smoke tests passed."
else
    echo "${failures} smoke test(s) failed."
    exit 1
fi
