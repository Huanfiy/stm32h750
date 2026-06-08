TOOLCHAIN ?= $(HOME)/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin
CC        := $(TOOLCHAIN)/arm-none-eabi-gcc
OBJDUMP   := $(TOOLCHAIN)/arm-none-eabi-objdump
NM        := $(TOOLCHAIN)/arm-none-eabi-nm

CFLAGS  := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 \
           -Os -g -ffunction-sections -fdata-sections \
           -fno-builtin -fno-common -std=c99 -Wall

LDFLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 \
           -T flashloader.lds \
           -nostartfiles -nostdlib -nodefaultlibs \
           -Wl,--gc-sections -Wl,-Map=flashloader.map \
           -Wl,--no-warn-rwx-segments

SRC := FlashPrg.c FlashDev.c

OUT := STM32H750VB_W25Q64.FLM

all: $(OUT)

$(OUT): $(SRC) FlashOS.h flashloader.lds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)
	@echo "--- Sections ---"
	@$(OBJDUMP) -h $@ | grep -E "PrgCode|PrgData|DevDscr"
	@echo "--- Symbols ---"
	@$(NM) $@ | grep -E "Init|UnInit|EraseSector|EraseChip|ProgramPage|SEGGER_OPEN_Read|FlashDevice|BlankCheck"

JLINK_USER_DIR := $(HOME)/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64

install: $(OUT) JLinkDevices.xml
	mkdir -p $(JLINK_USER_DIR)
	cp $(OUT) $(JLINK_USER_DIR)/
	cp JLinkDevices.xml $(JLINK_USER_DIR)/
	@echo "Installed to $(JLINK_USER_DIR)"

clean:
	rm -f $(OUT) flashloader.map

.PHONY: all clean install
