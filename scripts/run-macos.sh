#!/usr/bin/env bash
#
# Build (only when needed) and launch SDL Shader Studio on macOS.
#
#   ./scripts/run-macos.sh                 # build if stale, then start the app
#   ./scripts/run-macos.sh examples/hello  # ...and open that project
#   ./scripts/run-macos.sh --force         # rebuild even if nothing changed
#
# Every prerequisite is checked before anything is configured, so a missing
# tool is reported as itself rather than as a CMake error three minutes in.
set -euo pipefail

readonly REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${REPO_ROOT}/build"
readonly APP_BUNDLE="${BUILD_DIR}/bin/SS Studio.app"
readonly APP_BINARY="${APP_BUNDLE}/Contents/MacOS/SS Studio"
readonly MIN_CMAKE="3.21"

BUILD_TYPE="Release"
FORCE_BUILD=0
CLEAN=0
APP_ARGS=()

# --- output ------------------------------------------------------------------
if [ -t 2 ]; then
    readonly C_RED=$'\033[31m' C_YELLOW=$'\033[33m' C_BLUE=$'\033[34m' C_OFF=$'\033[0m'
else
    readonly C_RED='' C_YELLOW='' C_BLUE='' C_OFF=''
fi

info() { printf '%s==>%s %s\n' "${C_BLUE}" "${C_OFF}" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "${C_YELLOW}" "${C_OFF}" "$*" >&2; }

# Every environment problem ends here: say what is missing, say how to get it,
# and stop before the build can fail in a less obvious way.
die() {
    printf '%serror:%s %s\n' "${C_RED}" "${C_OFF}" "$1" >&2
    shift
    for line in "$@"; do printf '       %s\n' "${line}" >&2; done
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/run-macos.sh [options] [project path]

Builds SDL Shader Studio if the build is missing or out of date, then launches
it. Anything after the options is passed to the app; a project path there is
opened on startup.

options:
  -f, --force    build even when the existing binary is up to date
  -c, --clean    delete the build directory and configure from scratch
  -d, --debug    build Debug instead of Release
  -h, --help     show this message
EOF
}

# --- arguments ---------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force) FORCE_BUILD=1 ;;
        -c|--clean) CLEAN=1 ;;
        -d|--debug) BUILD_TYPE="Debug" ;;
        -h|--help) usage; exit 0 ;;
        --) shift; APP_ARGS+=("$@"); break ;;
        -*) die "unknown option '$1'" "Run 'scripts/run-macos.sh --help' for the list." ;;
        *) APP_ARGS+=("$1") ;;
    esac
    shift
done

# --- environment -------------------------------------------------------------
# "3.21" <= "4.3.0" without assuming sort -V is present.
version_at_least() {
    local have="$1" want="$2"
    [ "$(printf '%s\n%s\n' "${want}" "${have}" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "${want}" ]
}

check_environment() {
    [ "$(uname -s)" = "Darwin" ] ||
        die "this script is for macOS; you are on $(uname -s)." \
            "Use scripts/run-linux.sh or scripts/run-windows.ps1 instead."

    command -v cmake >/dev/null 2>&1 ||
        die "cmake is not installed (or not on PATH)." \
            "Install it with:  brew install cmake" \
            "Homebrew itself: https://brew.sh"

    local cmake_version
    cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
    version_at_least "${cmake_version}" "${MIN_CMAKE}" ||
        die "cmake ${cmake_version} is too old; ${MIN_CMAKE} or newer is required." \
            "Upgrade with:  brew upgrade cmake"

    command -v git >/dev/null 2>&1 ||
        die "git is not installed (or not on PATH)." \
            "CMake fetches SDL3, SDL_shadercross and toml++ with it when they" \
            "are not already installed." \
            "Install it with:  xcode-select --install"

    xcode-select --print-path >/dev/null 2>&1 ||
        die "the Xcode command line tools are not installed." \
            "Install them with:  xcode-select --install"

    command -v clang++ >/dev/null 2>&1 ||
        die "no C++ compiler found (clang++ is not on PATH)." \
            "Install the command line tools:  xcode-select --install" \
            "If Xcode is installed but not selected:" \
            "  sudo xcode-select --switch /Applications/Xcode.app"

    # Not fatal: the app builds, runs and packs SPIR-V/DXIL/MSL without full
    # Xcode. Only precompiled .metallib artifacts need `xcrun metal`.
    xcrun --find metal >/dev/null 2>&1 ||
        warn "'xcrun metal' is unavailable, so builds cannot emit .metallib artifacts. Everything else works; install full Xcode if you need them."
}

check_environment

# --- build -------------------------------------------------------------------
# True when the app has never been built, or when anything it is built from has
# changed since. Keeps the common case - run, look, run again - free of a build
# step that would have nothing to do.
needs_build() {
    [ -x "${APP_BINARY}" ] || return 0
    local newer
    newer="$(find "${REPO_ROOT}/CMakeLists.txt" "${REPO_ROOT}/cmake" "${REPO_ROOT}/src" \
                  "${REPO_ROOT}/include" "${REPO_ROOT}/templates" "${REPO_ROOT}/tools" \
                  -newer "${APP_BINARY}" -print -quit 2>/dev/null || true)"
    [ -n "${newer}" ]
}

if [ "${CLEAN}" -eq 1 ]; then
    info "removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    info "configuring (${BUILD_TYPE}); the first run fetches dependencies and takes a few minutes"
    cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ||
        die "cmake configure failed." \
            "The output above says what is missing. To start from a clean slate:" \
            "  scripts/run-macos.sh --clean"
fi

if [ "${FORCE_BUILD}" -eq 1 ] || needs_build; then
    info "building"
    cmake --build "${BUILD_DIR}" -j ||
        die "the build failed; see the compiler output above."
else
    info "build is up to date, skipping"
fi

[ -x "${APP_BINARY}" ] ||
    die "the build finished but no app was produced at:" \
        "  ${APP_BINARY}" \
        "The GUI is skipped when SDL3 cannot be found or built. Re-run with --clean," \
        "and check the configure output for 'SDL3 was not found'."

# --- launch ------------------------------------------------------------------
info "launching SDL Shader Studio"
exec "${APP_BINARY}" "${APP_ARGS[@]+"${APP_ARGS[@]}"}"
