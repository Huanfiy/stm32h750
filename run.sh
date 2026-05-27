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
readonly ELF_FILE="rtthread.elf"
readonly BIN_FILE="rtthread.bin"

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
    log_info "Cleaning..."
    scons -c -Q
    rm -f "$ELF_FILE" "$BIN_FILE" rtthread.map
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
  build         Build firmware (scons)
  clean         Clean build artifacts
  flash         Flash $BIN_FILE via J-Link SWD
  reset         Reset target via J-Link
  rebuild       Clean + build
  rebuild-flash Clean + build + flash
  help          Show this help

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
        build)          cmd_build ;;
        clean)          cmd_clean ;;
        flash)          cmd_flash ;;
        reset)          cmd_reset ;;
        rebuild)        cmd_clean; cmd_build ;;
        rebuild-flash)  cmd_clean; cmd_build; cmd_flash ;;
        help|--help|-h) show_help ;;
        *)              die "Unknown command: $cmd" ;;
    esac
}

main "$@"
