#!/usr/bin/env bash
set -Eeuo pipefail

MODULE="rtl8188_mon"
CLI_BIN="rtl8188_cli"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() {
    echo "[install] $*"
}

err() {
    echo "[install][error] $*" >&2
}

run_privileged() {
    if [[ "${EUID}" -eq 0 ]]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        err "Need root privileges (run as root or install sudo)."
        exit 1
    fi
}

install_deps_dnf_or_yum() {
    local pm="$1"
    local kernel_pkg="kernel-devel-$(uname -r)"

    log "Installing dependencies with ${pm}..."
    if ! run_privileged "$pm" install -y gcc make ncurses-devel iw pkgconf-pkg-config "$kernel_pkg"; then
        log "${kernel_pkg} not available, trying generic kernel-devel..."
        run_privileged "$pm" install -y gcc make ncurses-devel iw pkgconf-pkg-config kernel-devel
    fi
}

install_deps_apt() {
    local kernel_pkg="linux-headers-$(uname -r)"

    log "Updating apt package index..."
    run_privileged apt-get update

    log "Installing dependencies with apt-get..."
    if ! run_privileged apt-get install -y build-essential libncurses-dev iw pkg-config "$kernel_pkg"; then
        log "${kernel_pkg} not available, trying generic linux headers..."
        run_privileged apt-get install -y build-essential libncurses-dev iw pkg-config linux-headers-generic
    fi
}

install_dependencies() {
    if command -v dnf >/dev/null 2>&1; then
        install_deps_dnf_or_yum dnf
    elif command -v yum >/dev/null 2>&1; then
        install_deps_dnf_or_yum yum
    elif command -v apt-get >/dev/null 2>&1; then
        install_deps_apt
    else
        err "No supported package manager found (dnf/yum/apt-get)."
        err "Please install dependencies manually: gcc make kernel headers ncurses-devel iw"
        exit 1
    fi
}

show_build_state() {
    if [[ -f "${ROOT_DIR}/${MODULE}.ko" ]]; then
        log "Found existing module artifact: ${MODULE}.ko"
    else
        log "Module artifact not found: ${MODULE}.ko"
    fi

    if [[ -x "${ROOT_DIR}/${CLI_BIN}" ]]; then
        log "Found existing CLI artifact: ${CLI_BIN}"
    else
        log "CLI artifact not found: ${CLI_BIN}"
    fi
}

is_module_loaded() {
    lsmod | awk '{print $1}' | grep -qx "${MODULE}"
}

main() {
    cd "${ROOT_DIR}"

    log "Project directory: ${ROOT_DIR}"

    install_dependencies

    show_build_state

    log "Rebuilding kernel module..."
    make module

    log "Rebuilding CLI..."
    make cli

    if is_module_loaded; then
        log "Module ${MODULE} is currently loaded. Reloading..."
        run_privileged make reload
    else
        log "Module ${MODULE} is not loaded. Loading..."
        run_privileged make load
    fi

    log "Current status:"
    make status || true

    log "Done."
}

main "$@"
