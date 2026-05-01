#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${PROJECT_DIR}/build/linux"
CMAKE_GENERATOR=""
FASTNET_ENABLE_SSL="OFF"
FASTNET_BUILD_EXAMPLES="ON"
FASTNET_BUILD_TESTS="ON"
FASTNET_WARNINGS_AS_ERRORS="ON"
BUILD_RELEASE=1
BUILD_DEBUG=1
CLEAN_FIRST=0
RUN_TESTS=0
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-}"
EXTRA_CMAKE_ARGS=()

show_help() {
    cat <<EOF
Usage: ./build.sh [options] [extra-cmake-args]

Options:
  --clean          Remove Linux build directories before configuring
  --ssl            Enable FASTNET_ENABLE_SSL
  --no-examples    Disable example targets
  --no-tests       Disable test targets
  --no-werror      Disable FASTNET_WARNINGS_AS_ERRORS
  --release-only   Build Release only
  --debug-only     Build Debug only
  --build-dir DIR  Override Linux build root directory
  --generator GEN  Override the CMake generator
  --jobs N         Build with N parallel jobs
  --test           Run ctest after building each enabled configuration
  --help           Show this help

Extra arguments are forwarded directly to CMake configure.

Examples:
  ./build.sh --clean --release-only
  ./build.sh --ssl --release-only --test
  ./build.sh --no-examples --no-tests -DCMAKE_INSTALL_PREFIX=/usr/local
EOF
}

die() {
    echo "[ERROR] $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required but was not found in PATH."
}

remove_build_dir() {
    local dir="$1"
    if [[ -z "${dir}" || "${dir}" == "/" ]]; then
        die "Refusing to remove unsafe build directory: '${dir}'"
    fi
    if [[ -d "${dir}" ]]; then
        rm -rf -- "${dir}"
    fi
}

configure_and_build() {
    local config="$1"
    local build_dir="${BUILD_ROOT}/${config}"
    local generator_args=()
    local build_args=(--build "${build_dir}" --config "${config}")

    if [[ -n "${CMAKE_GENERATOR}" ]]; then
        generator_args=(-G "${CMAKE_GENERATOR}")
    fi

    if [[ "${CLEAN_FIRST}" -eq 1 ]]; then
        echo "[clean] ${build_dir}"
        remove_build_dir "${build_dir}"
    fi

    echo "[configure] ${config}"
    cmake -S "${PROJECT_DIR}" -B "${build_dir}" \
        "${generator_args[@]}" \
        -DCMAKE_BUILD_TYPE="${config}" \
        -DFASTNET_ENABLE_SSL="${FASTNET_ENABLE_SSL}" \
        -DFASTNET_BUILD_EXAMPLES="${FASTNET_BUILD_EXAMPLES}" \
        -DFASTNET_BUILD_TESTS="${FASTNET_BUILD_TESTS}" \
        -DFASTNET_WARNINGS_AS_ERRORS="${FASTNET_WARNINGS_AS_ERRORS}" \
        "${EXTRA_CMAKE_ARGS[@]}"

    echo "[build] ${config}"
    if [[ -n "${JOBS}" ]]; then
        build_args+=(--parallel "${JOBS}")
    else
        build_args+=(--parallel)
    fi
    cmake "${build_args[@]}"

    if [[ "${RUN_TESTS}" -eq 1 && "${FASTNET_BUILD_TESTS}" == "ON" ]]; then
        echo "[test] ${config}"
        (cd "${build_dir}" && ctest -C "${config}" --output-on-failure)
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            show_help
            exit 0
            ;;
        --clean)
            CLEAN_FIRST=1
            shift
            ;;
        --ssl)
            FASTNET_ENABLE_SSL="ON"
            shift
            ;;
        --no-examples)
            FASTNET_BUILD_EXAMPLES="OFF"
            shift
            ;;
        --no-tests)
            FASTNET_BUILD_TESTS="OFF"
            shift
            ;;
        --no-werror)
            FASTNET_WARNINGS_AS_ERRORS="OFF"
            shift
            ;;
        --release-only)
            BUILD_RELEASE=1
            BUILD_DEBUG=0
            shift
            ;;
        --debug-only)
            BUILD_RELEASE=0
            BUILD_DEBUG=1
            shift
            ;;
        --build-dir)
            shift
            [[ $# -gt 0 ]] || die "Missing value for --build-dir."
            BUILD_ROOT="$1"
            shift
            ;;
        --generator)
            shift
            [[ $# -gt 0 ]] || die "Missing value for --generator."
            CMAKE_GENERATOR="$1"
            shift
            ;;
        --jobs|-j)
            shift
            [[ $# -gt 0 ]] || die "Missing value for --jobs."
            JOBS="$1"
            shift
            ;;
        --test)
            RUN_TESTS=1
            shift
            ;;
        --)
            shift
            while [[ $# -gt 0 ]]; do
                EXTRA_CMAKE_ARGS+=("$1")
                shift
            done
            ;;
        *)
            EXTRA_CMAKE_ARGS+=("$1")
            shift
            ;;
    esac
done

[[ "${BUILD_RELEASE}" -eq 1 || "${BUILD_DEBUG}" -eq 1 ]] || die "No build configuration selected."

case "${BUILD_ROOT}" in
    /*) ;;
    *) BUILD_ROOT="${PROJECT_DIR}/${BUILD_ROOT}" ;;
esac

require_command cmake
if [[ "${RUN_TESTS}" -eq 1 && "${FASTNET_BUILD_TESTS}" == "ON" ]]; then
    require_command ctest
fi

if [[ -z "${CMAKE_GENERATOR}" ]] && command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR="Ninja"
fi

echo "========================================"
echo "  FastNet Linux Build Script"
echo "========================================"
echo "Project dir : ${PROJECT_DIR}"
echo "Build root  : ${BUILD_ROOT}"
echo "Generator   : ${CMAKE_GENERATOR:-CMake default}"
echo "SSL         : ${FASTNET_ENABLE_SSL}"
echo "Examples    : ${FASTNET_BUILD_EXAMPLES}"
echo "Tests       : ${FASTNET_BUILD_TESTS}"
echo "Werror      : ${FASTNET_WARNINGS_AS_ERRORS}"
echo "Run ctest   : ${RUN_TESTS}"

if [[ "${BUILD_RELEASE}" -eq 1 ]]; then
    configure_and_build "Release"
fi

if [[ "${BUILD_DEBUG}" -eq 1 ]]; then
    configure_and_build "Debug"
fi

echo "========================================"
echo "  Build completed successfully"
echo "========================================"
echo "Library dir : ${PROJECT_DIR}/lib"
echo "Runtime dir : ${PROJECT_DIR}/bin"
