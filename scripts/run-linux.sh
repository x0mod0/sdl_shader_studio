#!/usr/bin/env bash
#
# Build (only when needed) and launch SDL Shader Studio on Linux.
#
#   ./scripts/run-linux.sh                 # build if stale, then start the app
#   ./scripts/run-linux.sh examples/hello  # ...and open that project
#   ./scripts/run-linux.sh --force         # rebuild even if nothing changed
#
# Every prerequisite is checked before anything is configured, so a missing
# package is reported as itself rather than as a CMake error three minutes in.
set -euo pipefail

readonly REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${REPO_ROOT}/build"
readonly APP_BINARY="${BUILD_DIR}/bin/SS Studio"
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
usage: scripts/run-linux.sh [options] [project path]

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
        -*) die "unknown option '$1'" "Run 'scripts/run-linux.sh --help' for the list." ;;
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

# The install line that matches whichever package manager is actually here.
install_hint() {
    if command -v apt-get >/dev/null 2>&1; then
        printf 'sudo apt install %s' "$1"
    elif command -v dnf >/dev/null 2>&1; then
        printf 'sudo dnf install %s' "$2"
    elif command -v pacman >/dev/null 2>&1; then
        printf 'sudo pacman -S %s' "$3"
    else
        printf 'install with your package manager: %s' "$1"
    fi
}

check_environment() {
    [ "$(uname -s)" = "Linux" ] ||
        die "this script is for Linux; you are on $(uname -s)." \
            "Use scripts/run-macos.sh or scripts/run-windows.ps1 instead."

    command -v cmake >/dev/null 2>&1 ||
        die "cmake is not installed (or not on PATH)." \
            "  $(install_hint cmake cmake cmake)"

    local cmake_version
    cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
    version_at_least "${cmake_version}" "${MIN_CMAKE}" ||
        die "cmake ${cmake_version} is too old; ${MIN_CMAKE} or newer is required." \
            "Upgrade your distribution's package, or use a release from https://cmake.org/download/"

    command -v git >/dev/null 2>&1 ||
        die "git is not installed (or not on PATH)." \
            "CMake fetches SDL3, SDL_shadercross, Dear ImGui and toml++ with it when they" \
            "are not already installed." \
            "  $(install_hint git git git)"

    command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 ||
        command -v clang++ >/dev/null 2>&1 ||
        die "no C++ compiler found (none of c++, g++ or clang++ is on PATH)." \
            "This project needs C++20: GCC 12+ or Clang 15+." \
            "  $(install_hint build-essential gcc-c++ base-devel)"

    command -v make >/dev/null 2>&1 || command -v ninja >/dev/null 2>&1 ||
        die "no build tool found (neither make nor ninja is on PATH)." \
            "  $(install_hint make make make)"

    # SDL3 is either already installed or built from source here. A source build
    # needs the windowing development headers, and its failure message is a wall
    # of CMake output, so the headers are checked up front instead.
    if ! pkg-config --exists sdl3 2>/dev/null; then
        local have_headers=0
        for header in /usr/include/X11/Xlib.h /usr/include/wayland-client.h \
                      /usr/local/include/X11/Xlib.h /usr/local/include/wayland-client.h; do
            [ -f "${header}" ] && have_headers=1 && break
        done
        [ "${have_headers}" -eq 1 ] ||
            die "SDL3 is not installed and the headers needed to build it are missing." \
                "SDL3 needs X11 or Wayland development packages:" \
                "  $(install_hint 'libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev libvulkan-dev' \
                                  'libX11-devel wayland-devel libxkbcommon-devel mesa-libEGL-devel vulkan-devel' \
                                  'libx11 wayland libxkbcommon mesa vulkan-icd-loader')"
    fi

    # Not fatal: the editor, the compiler and the packer all work headless, and
    # the preview panel reports the missing device rather than crashing.
    command -v vulkaninfo >/dev/null 2>&1 || pkg-config --exists vulkan 2>/dev/null ||
        warn "no Vulkan loader detected. The app still starts and compiles shaders; the preview panel will report that no GPU device is available."
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
            "  scripts/run-linux.sh --clean"
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
[ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ] ||
    die "no display server is available (neither DISPLAY nor WAYLAND_DISPLAY is set)." \
        "SDL Shader Studio is a desktop app and needs one. Over SSH, connect with 'ssh -X'." \
        "The command line tool works headless:  ${BUILD_DIR}/bin/ssstudio --help"

info "launching SDL Shader Studio"
exec "${APP_BINARY}" "${APP_ARGS[@]+"${APP_ARGS[@]}"}"
