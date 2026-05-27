import os

ARCH     = 'arm'
CPU      = 'cortex-m7'
CROSS_TOOL = 'gcc'

BSP_LIBRARY_TYPE = None

if os.getenv('RTT_ROOT'):
    RTT_ROOT = os.getenv('RTT_ROOT')

PLATFORM  = 'gcc'
EXEC_PATH = os.getenv('RTT_EXEC_PATH', '/opt/gcc-arm-none-eabi/bin/')

BUILD = 'debug'

PREFIX  = 'arm-none-eabi-'
CC      = PREFIX + 'gcc'
AS      = PREFIX + 'gcc'
AR      = PREFIX + 'ar'
CXX     = PREFIX + 'g++'
LINK    = PREFIX + 'gcc'
TARGET_EXT = 'elf'
SIZE    = PREFIX + 'size'
OBJDUMP = PREFIX + 'objdump'
OBJCPY  = PREFIX + 'objcopy'

BUILD_DIR = 'build'
PROJECT_NAME = 'rtthread'
TARGET = os.path.join(BUILD_DIR, PROJECT_NAME + '.' + TARGET_EXT)

DEVICE = ' -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -ffunction-sections -fdata-sections'
CFLAGS = DEVICE + ' -Dgcc'
AFLAGS = ' -c' + DEVICE + ' -x assembler-with-cpp -Wa,-mimplicit-it=thumb '
LFLAGS = DEVICE + ' -Wl,--gc-sections,-Map=' + BUILD_DIR + '/' + PROJECT_NAME + '.map,-cref,-u,Reset_Handler -T board/linker_scripts/link.lds'

CPATH = ''
LPATH = ''

if BUILD == 'debug':
    CFLAGS += ' -O0 -gdwarf-2 -g'
    AFLAGS += ' -gdwarf-2'
else:
    CFLAGS += ' -O2'

CXXFLAGS = CFLAGS
CFLAGS += ' -std=c99'

POST_ACTION = OBJCPY + ' -O binary $TARGET ' + BUILD_DIR + '/' + PROJECT_NAME + '.bin\n' + \
              OBJDUMP + ' -D $TARGET > ' + BUILD_DIR + '/' + PROJECT_NAME + '.asm\n' + \
              SIZE + ' $TARGET\n'
