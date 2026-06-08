#!/bin/bash
set -o pipefail

# ==============================================================================
# Configuration
# ==============================================================================
readonly RTT_EXEC_PATH="${RTT_EXEC_PATH:-$HOME/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin}"
readonly JLINK_DEVICE="STM32H750VB"
readonly JLINK_INTF="SWD"
readonly JLINK_SPEED="${JLINK_SPEED:-8000}"
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
cmd_build_app() {
    check_toolchain

    local scons_args=(-j$(nproc) -Q)
    if [[ "$VERBOSE" -eq 0 ]]; then
        scons_args+=("--silent")
    fi

    log_info "Building app..."
    bear --output compile_commands.json -- scons "${scons_args[@]}"
    local rc=$?

    update_compile_commands

    [[ $rc -ne 0 ]] && die "App build failed."
    log_success "App build complete: $BIN_FILE"
}

cmd_clean_app() {
    check_toolchain
    log_info "Cleaning app..."
    scons -c -Q
    rm -rf "$BUILD_DIR"
    log_success "App clean complete."
}

cmd_app_flash() {
    [[ -f "$BIN_FILE" ]] || die "$BIN_FILE not found. Run 'build-app' first."

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

cmd_build_bl() {
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

cmd_clean_bl() {
    log_info "Cleaning bootloader..."
    make -C "$BOOT_DIR" clean --no-print-directory
    log_success "Bootloader clean complete."
}

cmd_flash_bl() {
    [[ -f "$BOOT_BIN_FILE" ]] || die "$BOOT_BIN_FILE not found. Run 'build-bl' first."

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

cmd_build_all() {
    cmd_build_bl
    cmd_build_app
}

cmd_clean_all() {
    cmd_clean_bl
    cmd_clean_app
}

cmd_flash_app() {
    cmd_app_flash
}

cmd_flash_all() {
    cmd_flash_bl
    cmd_flash_app
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
  build              Build bootloader + app
  rebuild            Clean + build bootloader + app
  flash              Flash bootloader + app
  rebuild-flash      Clean + build + flash bootloader + app
  clean              Clean bootloader + app artifacts

  build-bl           Build bootloader ($BOOT_BIN_FILE)
  rebuild-bl         Clean + build bootloader
  flash-bl           Flash bootloader to $FLASH_ADDR
  rebuild-flash-bl   Clean + build + flash bootloader
  clean-bl           Clean bootloader artifacts

  build-app          Build app ($BIN_FILE)
  rebuild-app        Clean + build app
  flash-app          Flash app to $APP_FLASH_ADDR (external W25Q64)
  rebuild-flash-app  Clean + build + flash app
  clean-app          Clean app artifacts

  reset              Reset target via J-Link
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
        build)               cmd_build_all ;;
        rebuild)             cmd_clean_all; cmd_build_all ;;
        flash)               cmd_flash_all ;;
        rebuild-flash)       cmd_clean_all; cmd_build_all; cmd_flash_all ;;
        clean)               cmd_clean_all ;;

        build-bl)            cmd_build_bl ;;
        rebuild-bl)          cmd_clean_bl; cmd_build_bl ;;
        flash-bl)            cmd_flash_bl ;;
        rebuild-flash-bl)    cmd_clean_bl; cmd_build_bl; cmd_flash_bl ;;
        clean-bl)            cmd_clean_bl ;;

        build-app)           cmd_build_app ;;
        rebuild-app)         cmd_clean_app; cmd_build_app ;;
        flash-app)           cmd_flash_app ;;
        rebuild-flash-app)   cmd_clean_app; cmd_build_app; cmd_flash_app ;;
        clean-app)           cmd_clean_app ;;

        reset)               cmd_reset ;;
        help|--help|-h)      show_help ;;
        *)                   die "Unknown command: $cmd. Run '$0 help' for available commands." ;;
    esac
}

main "$@"
