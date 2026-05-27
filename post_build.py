#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Post-build script for firmware information display
"""

import os
import sys
import re
import subprocess


class Colors:
    RED = '\033[91m'
    YELLOW = '\033[93m'
    GREEN = '\033[92m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    BOLD = '\033[1m'
    END = '\033[0m'


def run_command(cmd):
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.stdout.strip(), result.stderr.strip(), result.returncode
    except Exception as e:
        return "", str(e), 1


def get_file_size(filepath):
    try:
        return os.path.getsize(filepath)
    except:
        return 0


def format_size(size_bytes):
    if size_bytes < 1024:
        return f"{size_bytes:>4}B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes/1024:>5.1f}K"
    else:
        return f"{size_bytes/(1024*1024):>5.2f}M"


def parse_size_output(size_output):
    lines = size_output.strip().split('\n')
    if len(lines) < 2:
        return None

    for line in lines[1:]:
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit():
            return {
                'text': int(parts[0]),
                'data': int(parts[1]),
                'bss': int(parts[2]),
                'total': int(parts[3])
            }
    return None


def create_progress_bar(percentage, width=30):
    filled = int(width * percentage / 100)
    bar = '=' * filled
    if filled < width:
        bar += '>'
        bar += ' ' * (width - filled - 1)
    else:
        bar = '=' * width
    return f"[{bar}]"


def parse_linker_memory(ldscript_path):
    with open(ldscript_path, 'r') as f:
        content = f.read()

    regions = {}
    pattern = r'(\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*(?:0x[\da-fA-F]+|\d+)\s*,\s*LENGTH\s*=\s*(\d+)(K|M)?'
    for m in re.finditer(pattern, content):
        name = m.group(1)
        length = int(m.group(2))
        suffix = m.group(3)
        if suffix == 'K':
            length *= 1024
        elif suffix == 'M':
            length *= 1024 * 1024
        regions[name] = length
    return regions


def print_firmware_info(elf_file, bin_file, mode_hint, ldscript):
    regions = parse_linker_memory(ldscript)
    FLASH_SIZE = regions.get('ROM', 1920 * 1024)
    RAM_SIZE = regions.get('RAM', 128 * 1024)

    size_cmd = f"arm-none-eabi-size --format=berkeley {elf_file}"
    size_output, size_error, size_ret = run_command(size_cmd)

    if size_ret != 0:
        print(f"{Colors.RED}Failed to get firmware info: {size_error}{Colors.END}")
        return

    size_info = parse_size_output(size_output)
    if not size_info:
        print(f"{Colors.RED}Failed to parse firmware info{Colors.END}")
        return

    flash_used = size_info['text'] + size_info['data']
    ram_used = size_info['data'] + size_info['bss']

    flash_pct = (flash_used / FLASH_SIZE) * 100
    ram_pct = (ram_used / RAM_SIZE) * 100

    bin_size = get_file_size(bin_file)
    elf_size = get_file_size(elf_file)

    build_mode = mode_hint

    print("=" * 70)
    mode_color = Colors.BLUE if build_mode == "Debug" else Colors.GREEN
    print(f"{Colors.BOLD}{Colors.CYAN}STM32H750 Firmware Information{Colors.END} -- {mode_color}{build_mode}{Colors.END}")
    print("=" * 70)

    print(f"{Colors.BOLD}Memory Usage:{Colors.END}")
    print(f"Flash: {format_size(flash_used)} / {format_size(FLASH_SIZE)} ({flash_pct:5.1f}%) {create_progress_bar(flash_pct)}")
    print(f"RAM  : {format_size(ram_used)} / {format_size(RAM_SIZE)} ({ram_pct:5.1f}%) {create_progress_bar(ram_pct)}")

    print()
    print(f"{Colors.BOLD}Section Details:{Colors.END}")
    print(f"ELF:{format_size(elf_size)}  BIN:{format_size(bin_size)} TEXT:{format_size(size_info['text'])}  DATA:{format_size(size_info['data'])}  BSS:{format_size(size_info['bss'])}")

    print()
    if flash_pct > 90 or ram_pct > 90:
        print(f"{Colors.RED}{Colors.BOLD}WARNING: Memory usage is critically high!{Colors.END}")
    elif flash_pct > 80 or ram_pct > 80:
        print(f"{Colors.YELLOW}{Colors.BOLD}NOTICE: Memory usage is high{Colors.END}")
    else:
        print(f"{Colors.GREEN}{Colors.BOLD}Memory usage is normal{Colors.END}")

    print("=" * 70)


def main():
    usage = "Usage: python3 post_build.py --mode=Debug|Release --ldscript=<path> <elf_file>"

    named_args = {}
    positional = []
    for arg in sys.argv[1:]:
        if arg.startswith('--') and '=' in arg:
            key, val = arg.split('=', 1)
            named_args[key] = val
        else:
            positional.append(arg)

    mode_hint = named_args.get('--mode')
    ldscript = named_args.get('--ldscript')

    if not mode_hint or mode_hint not in ("Debug", "Release"):
        print(usage)
        sys.exit(1)

    if not ldscript or not os.path.exists(ldscript):
        print(f"{Colors.RED}Linker script not found: {ldscript}{Colors.END}")
        print(usage)
        sys.exit(1)

    if len(positional) != 1:
        print(usage)
        sys.exit(1)

    elf_file = positional[0]
    bin_file = elf_file.replace('.elf', '.bin')

    if not os.path.exists(bin_file):
        bin_file = os.path.join('build', os.path.basename(bin_file))

    if not os.path.exists(elf_file):
        print(f"{Colors.RED}ELF file not found: {elf_file}{Colors.END}")
        sys.exit(1)

    if not os.path.exists(bin_file):
        print(f"{Colors.RED}BIN file not found: {bin_file}{Colors.END}")
        sys.exit(1)

    print_firmware_info(elf_file, bin_file, mode_hint, ldscript)


if __name__ == "__main__":
    main()
