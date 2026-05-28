#!/bin/bash
set -o pipefail

# ==============================================================================
# Configuration
# ==============================================================================
readonly RTT_EXEC_PATH="${RTT_EXEC_PATH:-$HOME/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin}"
readonly JLINK_DEVICE="STM32H750VB"
readonly JLINK_INTF="SWD"
readonly JLINK_SPEED="4000"
readonly FLASH_ADDR="0x08000000"
readonly BUILD_DIR="build"
readonly PROJECT_NAME="${PROJECT_NAME:-rt-thread}"
readonly ELF_FILE="${BUILD_DIR}/${PROJECT_NAME}.elf"
readonly BIN_FILE="${BUILD_DIR}/${PROJECT_NAME}.bin"

readonly BOOT_DIR="bootloader"
readonly BOOT_BUILD_DIR="${BOOT_DIR}/build"
readonly BOOT_ELF_FILE="${BOOT_BUILD_DIR}/bootloader.elf"
readonly BOOT_BIN_FILE="${BOOT_BUILD_DIR}/bootloader.bin"

readonly APP_FLASH_ADDR="0x90000000"
# QSPI flash bank is merged into the built-in STM32H750VB device profile via
# ~/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64/JLinkDevices.xml. The
# device name therefore stays the same as the bootloader -- only the second
# flash bank loader changes.
readonly APP_JLINK_DEVICE="$JLINK_DEVICE"

readonly R='\033[0;31m'
readonly G='\033[0;32m'
readonly B='\033[0;34m'
readonly NC='\033[0m'

VERBOSE=0

# ==============================================================================
# Helpers
# ==============================================================================
log_info()    { echo -e "${B}[INFO]${NC} $1"; }
log_success() { echo -e "${G}[OK]${NC} $1"; }
log_error()   { echo -e "${R}[ERROR]${NC} $1" >&2; }
die()         { log_error "$1"; exit 1; }

check_toolchain() {
    [[ -d "$RTT_EXEC_PATH" ]] || die "Toolchain not found: $RTT_EXEC_PATH"
    export RTT_EXEC_PATH
}

update_compile_commands() {
    if [[ -f "compile_commands.json" ]] && grep -q '"output"' "compile_commands.json"; then
        mkdir -p .vscode
        mv compile_commands.json .vscode/
        log_info "Updated compile_commands.json"
    else
        [[ -f "compile_commands.json" ]] && rm -f compile_commands.json
    fi
}

# ==============================================================================
# Commands
# ==============================================================================
cmd_build() {
    check_toolchain

    local scons_args=(-j$(nproc) -Q)
    if [[ "$VERBOSE" -eq 0 ]]; then
        scons_args+=("--silent")
    fi

    log_info "Building..."
    bear --output compile_commands.json -- scons "${scons_args[@]}"
    local rc=$?

    update_compile_commands

    [[ $rc -ne 0 ]] && die "Build failed."
    log_success "Build complete."
}

cmd_clean() {
    check_toolchain
    log_info "Cleaning..."
    scons -c -Q
    rm -rf "$BUILD_DIR"
    log_success "Clean complete."
}

cmd_flash() {
    [[ -f "$BIN_FILE" ]] || die "$BIN_FILE not found. Build first."

    log_info "Flashing $BIN_FILE to $FLASH_ADDR via J-Link $JLINK_INTF..."

    local jlink_script
    jlink_script=$(mktemp)
    cat > "$jlink_script" <<EOF
si $JLINK_INTF
speed $JLINK_SPEED
device $JLINK_DEVICE
connect
h
loadbin $BIN_FILE, $FLASH_ADDR
r
g
exit
EOF

    if JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$jlink_script" > /dev/null 2>&1; then
        log_success "Flash complete."
    else
        rm -f "$jlink_script"
        die "Flash failed."
    fi
    rm -f "$jlink_script"
}

cmd_app_flash() {
    [[ -f "$BIN_FILE" ]] || die "$BIN_FILE not found. Run 'build' first."

    local devfile="$HOME/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64/STM32H750VB_W25Q64.FLM"
    [[ -f "$devfile" ]] || die "$devfile not found. Run 'make -C tools/flashloader install' first."

    log_info "Flashing $BIN_FILE to $APP_FLASH_ADDR (W25Q64 via custom Open Flashloader)..."

    local jlink_script
    jlink_script=$(mktemp)
    cat > "$jlink_script" <<EOF
si $JLINK_INTF
speed $JLINK_SPEED
device $APP_JLINK_DEVICE
connect
h
loadbin $BIN_FILE, $APP_FLASH_ADDR
exit
EOF

    JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$jlink_script" > /tmp/app-flash.log 2>&1
    rm -f "$jlink_script"

    if grep -qi "verification failed\|flashing failed\|error:" /tmp/app-flash.log; then
        tail -25 /tmp/app-flash.log
        die "App flash to QSPI failed -- see /tmp/app-flash.log."
    fi

    if grep -qE "Flash download: Bank|Contents already match" /tmp/app-flash.log; then
        log_success "App flash to QSPI complete."
        grep -E "Flash download:|Contents already match" /tmp/app-flash.log | sed 's/^/    /'
    else
        tail -25 /tmp/app-flash.log
        die "App flash result unclear -- see /tmp/app-flash.log."
    fi
}

cmd_boot_build() {
    check_toolchain

    local make_args=(-C "$BOOT_DIR" -j"$(nproc)")
    if [[ "$VERBOSE" -eq 0 ]]; then
        make_args+=(--no-print-directory -s)
    fi

    log_info "Building bootloader..."
    make "${make_args[@]}"
    local rc=$?
    [[ $rc -ne 0 ]] && die "Bootloader build failed."
    log_success "Bootloader build complete: $BOOT_BIN_FILE"
}

cmd_boot_clean() {
    log_info "Cleaning bootloader..."
    make -C "$BOOT_DIR" clean --no-print-directory
    log_success "Bootloader clean complete."
}

cmd_boot_flash() {
    [[ -f "$BOOT_BIN_FILE" ]] || die "$BOOT_BIN_FILE not found. Run 'boot-build' first."

    log_info "Flashing $BOOT_BIN_FILE to $FLASH_ADDR via J-Link $JLINK_INTF..."

    local jlink_script
    jlink_script=$(mktemp)
    cat > "$jlink_script" <<EOF
si $JLINK_INTF
speed $JLINK_SPEED
device $JLINK_DEVICE
connect
h
loadbin $BOOT_BIN_FILE, $FLASH_ADDR
r
g
exit
EOF

    if JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$jlink_script" > /dev/null 2>&1; then
        log_success "Bootloader flash complete."
    else
        rm -f "$jlink_script"
        die "Bootloader flash failed."
    fi
    rm -f "$jlink_script"
}

cmd_reset() {
    log_info "Resetting target..."

    local jlink_script
    jlink_script=$(mktemp)
    cat > "$jlink_script" <<EOF
si $JLINK_INTF
speed $JLINK_SPEED
device $JLINK_DEVICE
connect
r
g
exit
EOF

    JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$jlink_script" > /dev/null 2>&1
    rm -f "$jlink_script"
    log_success "Reset complete."
}

show_help() {
    cat <<EOF
Usage: $0 <command> [options]

Commands:
  build              Build firmware (scons)
  clean              Clean build artifacts
  flash              Flash $BIN_FILE to $FLASH_ADDR (internal flash; legacy)
  reset              Reset target via J-Link
  rebuild            Clean + build
  rebuild-flash      Clean + build + flash (internal flash; legacy)
  app-flash          Flash $BIN_FILE to $APP_FLASH_ADDR (external W25Q64)
  app-rebuild-flash  Clean + build + app-flash
  boot-build         Build bootloader ($BOOT_BIN_FILE)
  boot-clean         Clean bootloader artifacts
  boot-flash         Flash $BOOT_BIN_FILE to $FLASH_ADDR (internal flash)
  boot-rebuild       Clean + build bootloader
  boot-rebuild-flash Clean + build + flash bootloader
  help               Show this help

Options:
  --verbose, -v   Show full SCons output
EOF
}

# ==============================================================================
# Main
# ==============================================================================
main() {
    [[ $# -eq 0 ]] && { show_help; exit 1; }

    local cmd=""
    for arg in "$@"; do
        case "$arg" in
            --verbose|-v) VERBOSE=1 ;;
            -*) die "Unknown option: $arg" ;;
            *)  [[ -z "$cmd" ]] && cmd="$arg" || die "Unexpected argument: $arg" ;;
        esac
    done

    case "$cmd" in
        build)               cmd_build ;;
        clean)               cmd_clean ;;
        flash)               cmd_flash ;;
        reset)               cmd_reset ;;
        rebuild)             cmd_clean; cmd_build ;;
        rebuild-flash)       cmd_clean; cmd_build; cmd_flash ;;
        app-flash)           cmd_app_flash ;;
        app-rebuild-flash)   cmd_clean; cmd_build; cmd_app_flash ;;
        boot-build)          cmd_boot_build ;;
        boot-clean)          cmd_boot_clean ;;
        boot-flash)          cmd_boot_flash ;;
        boot-rebuild)        cmd_boot_clean; cmd_boot_build ;;
        boot-rebuild-flash)  cmd_boot_clean; cmd_boot_build; cmd_boot_flash ;;
        help|--help|-h)      show_help ;;
        *)                   die "Unknown command: $cmd" ;;
    esac
}

main "$@"
