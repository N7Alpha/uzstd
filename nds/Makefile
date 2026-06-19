TARGET := uzstd_nds_bench
BUILD := build

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=/opt/devkitpro; export DEVKITARM=$$DEVKITPRO/devkitARM")
endif

DEVKITPRO ?= $(patsubst %/devkitARM,%,$(DEVKITARM))

CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
OBJCOPY := $(DEVKITARM)/bin/arm-none-eabi-objcopy
NDSTOOL := $(DEVKITPRO)/tools/bin/ndstool

ARCH9 := -marm -mcpu=arm946e-s -mtune=arm946e-s
ARCH7 := -marm -mcpu=arm7tdmi
COMMON_CFLAGS := -std=c99 -ffreestanding -fno-builtin -fomit-frame-pointer \
                 -ffunction-sections -fdata-sections -Wall -Wextra -O2
LDFLAGS_COMMON := -nostdlib -Wl,--gc-sections

ARM9_ELF := $(BUILD)/arm9.elf
ARM7_ELF := $(BUILD)/arm7.elf
ARM9_OBJS := $(BUILD)/arm9_start.o $(BUILD)/main.o
ARM7_OBJS := $(BUILD)/arm7_start.o

.PHONY: all clean

all: $(TARGET).nds

$(BUILD):
	@mkdir -p $@

$(BUILD)/main.o: source/main.c ../uzstd.h | $(BUILD)
	$(CC) $(COMMON_CFLAGS) $(ARCH9) -D__NDS__ -DARM9 -DNDS_BENCH_CONSOLE=0 -c $< -o $@

$(BUILD)/arm9_start.o: source/arm9_start.S | $(BUILD)
	$(CC) $(ARCH9) -c $< -o $@

$(BUILD)/arm7_start.o: source/arm7_start.S | $(BUILD)
	$(CC) $(ARCH7) -c $< -o $@

$(ARM9_ELF): $(ARM9_OBJS) arm9.ld
	$(CC) $(ARCH9) $(LDFLAGS_COMMON) -T arm9.ld $(ARM9_OBJS) -lgcc -o $@

$(ARM7_ELF): $(ARM7_OBJS) arm7.ld
	$(CC) $(ARCH7) $(LDFLAGS_COMMON) -T arm7.ld $(ARM7_OBJS) -lgcc -o $@

$(TARGET).nds: $(ARM9_ELF) $(ARM7_ELF)
	$(NDSTOOL) -c $@ -h 0x200 -9 $(ARM9_ELF) -7 $(ARM7_ELF)

clean:
	rm -rf $(BUILD) $(TARGET).nds $(TARGET).elf $(TARGET).arm9 $(TARGET).ds.gba
