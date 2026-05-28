# ============================================================================
# STM32H750 bootloader build (bare HAL, no RT-Thread).
#
# Replaces the original SConstruct + rtconfig.py pair. The bootloader does not
# use any RT-Thread BSP scaffolding (PrepareBuilding / DefineGroup), so the
# value of SCons here was nil; this layout mirrors tools/flashloader/Makefile
# so all "small bare-metal" build products in the repo use the same engine.
#
# Invoke as `make -C bootloader -j$(nproc)` (or via `./run.sh boot-build`).
# ============================================================================

PROJECT   := bootloader
BUILD_DIR := build
ELF       := $(BUILD_DIR)/$(PROJECT).elf
BIN       := $(BUILD_DIR)/$(PROJECT).bin
MAP       := $(BUILD_DIR)/$(PROJECT).map

# ---- Toolchain (RTT_EXEC_PATH from env > built-in default) -----------------
TOOLCHAIN ?= $(RTT_EXEC_PATH)
ifeq ($(strip $(TOOLCHAIN)),)
  TOOLCHAIN := $(HOME)/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin
endif
ifeq ($(wildcard $(TOOLCHAIN)/arm-none-eabi-gcc),)
  $(error arm-none-eabi-gcc not found under $(TOOLCHAIN); set RTT_EXEC_PATH or TOOLCHAIN)
endif

CC      := $(TOOLCHAIN)/arm-none-eabi-gcc
AS      := $(TOOLCHAIN)/arm-none-eabi-gcc
LD      := $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY := $(TOOLCHAIN)/arm-none-eabi-objcopy
SIZE    := $(TOOLCHAIN)/arm-none-eabi-size

# ---- Paths ----------------------------------------------------------------
ROOT      := ..
HAL_ROOT  := $(ROOT)/libraries/STM32H7xx_HAL
HAL_DRV   := $(HAL_ROOT)/STM32H7xx_HAL_Driver
CMSIS_DEV := $(HAL_ROOT)/CMSIS/Device/ST/STM32H7xx
CUBE_CFG  := $(ROOT)/board/CubeMX_Config

# ---- Flags ----------------------------------------------------------------
CPU_FLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
DEFINES   := -DSTM32H750xx -DUSE_HAL_DRIVER

INCS := -Iinc \
        -I$(CUBE_CFG)/Core/Inc \
        -I$(HAL_DRV)/Inc \
        -I$(HAL_DRV)/Inc/Legacy \
        -I$(ROOT)/libraries/HAL_Drivers/CMSIS/Include \
        -I$(CMSIS_DEV)/Include

CFLAGS  := $(CPU_FLAGS) -ffunction-sections -fdata-sections -std=c99 -Wall -Os -g
ASFLAGS := $(CPU_FLAGS) -x assembler-with-cpp -Wa,-mimplicit-it=thumb -g

LDSCRIPT := linker_scripts/boot.lds
LDFLAGS  := $(CPU_FLAGS) \
            -T$(LDSCRIPT) \
            -Wl,--gc-sections \
            -Wl,-Map=$(MAP) \
            -Wl,-cref \
            -Wl,-u,Reset_Handler \
            --specs=nano.specs --specs=nosys.specs \
            -g

# ---- Sources --------------------------------------------------------------
LOCAL_SRC := $(wildcard src/*.c)

# Minimum HAL modules required by the bootloader (UART/QSPI/RCC/PWR/GPIO/DMA/
# MDMA/EXTI/CORTEX). Hand-curated -- avoids dragging in ADC/SDMMC/USB etc.
HAL_MODULES := \
    stm32h7xx_hal.c \
    stm32h7xx_hal_cortex.c \
    stm32h7xx_hal_rcc.c \
    stm32h7xx_hal_rcc_ex.c \
    stm32h7xx_hal_pwr.c \
    stm32h7xx_hal_pwr_ex.c \
    stm32h7xx_hal_gpio.c \
    stm32h7xx_hal_uart.c \
    stm32h7xx_hal_uart_ex.c \
    stm32h7xx_hal_qspi.c \
    stm32h7xx_hal_dma.c \
    stm32h7xx_hal_dma_ex.c \
    stm32h7xx_hal_mdma.c \
    stm32h7xx_hal_exti.c
HAL_SRC := $(addprefix $(HAL_DRV)/Src/, $(HAL_MODULES))

EXT_SRC := $(HAL_SRC) $(CUBE_CFG)/Core/Src/system_stm32h7xx.c
EXT_ASM := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32h750xx.s

# ---- Object paths ----------------------------------------------------------
# Uses the .c.o / .s.o double-extension naming the previous SCons build used,
# so existing debug recipes that grep object paths keep working.
OBJ_DIR := $(BUILD_DIR)/objs

LOCAL_OBJS := $(LOCAL_SRC:src/%.c=$(OBJ_DIR)/src/%.c.o)
EXT_OBJS   := $(patsubst $(ROOT)/%,$(OBJ_DIR)/%.o,$(EXT_SRC))
ASM_OBJ    := $(patsubst $(ROOT)/%,$(OBJ_DIR)/%.o,$(EXT_ASM))

OBJS := $(LOCAL_OBJS) $(EXT_OBJS) $(ASM_OBJ)
DEPS := $(OBJS:.o=.d)

# ============================================================================
# Targets
# ============================================================================
.PHONY: all clean
.DEFAULT_GOAL := all

all: $(BIN)
	@$(SIZE) $(ELF)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(OBJS) $(LDFLAGS) -o $@

# Pattern rules per source root. Each one creates the nested objs/ directory
# on demand so we don't need an explicit pre-build mkdir target.

$(OBJ_DIR)/src/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(DEFINES) $(INCS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/libraries/%.c.o: $(ROOT)/libraries/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(DEFINES) $(INCS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/board/%.c.o: $(ROOT)/board/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(DEFINES) $(INCS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/libraries/%.s.o: $(ROOT)/libraries/%.s
	@mkdir -p $(dir $@)
	$(AS) -MMD -MP $(DEFINES) $(INCS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
