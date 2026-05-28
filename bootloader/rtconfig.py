"""
Bootloader build config. Mirrors the main project's toolchain selection but
emits artifacts under build_boot/ and links with bootloader/linker_scripts/boot.lds.
"""

import os

PROJECT_NAME = 'bootloader'
BUILD_DIR    = 'build_boot'
LDSCRIPT     = 'linker_scripts/boot.lds'

# Toolchain (matches ../rtconfig.py)
PREFIX = 'arm-none-eabi-'
CC     = PREFIX + 'gcc'
AS     = PREFIX + 'gcc'
AR     = PREFIX + 'ar'
LINK   = PREFIX + 'gcc'
OBJCPY = PREFIX + 'objcopy'
SIZE   = PREFIX + 'size'

EXEC_PATH = os.getenv('RTT_EXEC_PATH', '')

CPU_FLAGS = '-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16'

CFLAGS = (
    f'{CPU_FLAGS} '
    '-ffunction-sections -fdata-sections '
    '-std=c99 '
    '-Wall '
    '-Os -g '
)

AFLAGS = (
    f'-c {CPU_FLAGS} '
    '-x assembler-with-cpp '
    '-Wa,-mimplicit-it=thumb '
    '-g '
)

LFLAGS = (
    f'-T{LDSCRIPT} '
    '-Wl,--gc-sections '
    f'-Wl,-Map={BUILD_DIR}/{PROJECT_NAME}.map '
    '-Wl,-cref '
    '-Wl,-u,Reset_Handler '
    f'{CPU_FLAGS} '
    '--specs=nano.specs --specs=nosys.specs '
    '-g '
)

CPPDEFINES = ['STM32H750xx', 'USE_HAL_DRIVER']
