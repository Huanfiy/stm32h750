import os

PROJECT_NAME = os.getenv('PROJECT_NAME', 'rt-thread')

# toolchains options
ARCH = 'arm'
CPU = 'cortex-m7'
CROSS_TOOL = 'gcc'

PLATFORM = 'gcc'
EXEC_PATH = ''

if os.getenv('RTT_EXEC_PATH'):
    EXEC_PATH = os.getenv('RTT_EXEC_PATH')

PREFIX = 'arm-none-eabi-'
CC = PREFIX + 'gcc'
AS = PREFIX + 'gcc'
AR = PREFIX + 'ar'
CXX = PREFIX + 'g++'
LINK = PREFIX + 'gcc'
TARGET_EXT = 'elf'
SIZE = PREFIX + 'size'
OBJDUMP = PREFIX + 'objdump'
OBJCPY = PREFIX + 'objcopy'
DEVICE = ''
CFLAGS = ''
AFLAGS = ''
LFLAGS = ''
CPATH = ''
LPATH = ''
CXXFLAGS = ''
POST_ACTION = ''

BSP_LIBRARY_TYPE = None

BUILD_DIR = 'build'
TARGET = os.path.join(BUILD_DIR, PROJECT_NAME + '.' + TARGET_EXT)


#=========================#
# my custom configuration #
#=========================#

DEVICE = 'STM32H750'

CFLAGS = [
    '-mcpu=cortex-m7',
    '-mthumb',
    '-mfloat-abi=hard',
    '-mfpu=fpv5-d16',
    '-ffunction-sections -fdata-sections',
    '-std=c99',
    '-Dgcc',
]

AFLAGS = [
    '-c',
    '-mcpu=cortex-m7',
    '-mthumb',
    '-mfloat-abi=hard',
    '-mfpu=fpv5-d16',
    '-x assembler-with-cpp',
    '-Wa,-mimplicit-it=thumb',
]

LDSCRIPT = 'board/linker_scripts/link.lds'

LFLAGS = [
    f'-T{LDSCRIPT}',
    '-Wl,--gc-sections',
    f'-Wl,-Map=build/{PROJECT_NAME}.map',
    '-Wl,-cref',
    '-Wl,-u,Reset_Handler',
    '-mcpu=cortex-m7',
    '-mthumb',
    '-mfloat-abi=hard',
    '-mfpu=fpv5-d16',
    '--specs=nano.specs --specs=nosys.specs',
]

build_mode = os.getenv('BUILD_MODE', 'Debug')
if build_mode == 'Debug':
    CFLAGS.append('-O0')
    CFLAGS.append('-gdwarf-2 -g')
    AFLAGS.append('-gdwarf-2')
    LFLAGS.append('-g')
    print('debug mode')
else:
    CFLAGS.append('-O2')
    CFLAGS.append('-DNDEBUG')
    print('release mode')

CFLAGS = ' '.join(CFLAGS)
AFLAGS = ' '.join(AFLAGS)
LFLAGS = ' '.join(LFLAGS)

CXXFLAGS = CFLAGS

build_mode_arg = '--mode=Debug' if build_mode == 'Debug' else '--mode=Release'
POST_ACTION = f'{OBJCPY} -O binary $TARGET build/{PROJECT_NAME}.bin\n' + \
              f'{OBJDUMP} -D $TARGET > build/{PROJECT_NAME}.asm\n' + \
              f'python post_build.py {build_mode_arg} --ldscript={LDSCRIPT} $TARGET'
