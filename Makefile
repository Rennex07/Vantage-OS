CC = aarch64-linux-gnu-gcc
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

BUILD_DIR ?= out
.DEFAULT_GOAL := all
PYTHON ?= python

include mk/config.mk
include mk/sources.mk

OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
OBJECTS += $(patsubst %.s,$(BUILD_DIR)/%.o,$(S_SOURCES))
# The user program brings its own copy of the freestanding memory routines.
USER_OBJECTS := $(patsubst %.s,$(BUILD_DIR)/%.o,$(USER_ASM_SOURCES))
USER_OBJECTS += $(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_SOURCES)) $(BUILD_DIR)/lib/memory.o

CFLAGS = -Wall -O2 -ffreestanding -nostdinc
# Firmware discovery runs with the MMU off: unaligned Device accesses can fault.
CFLAGS += -mstrict-align -MMD -MP
BUILD_CONFIG_FLAGS := $(CPPFLAGS) $(CFLAGS)
STRICT_SOURCES := $(MEMORY_SOURCES) $(FILESYSTEM_SOURCES) $(SCHEDULER_SOURCES) \
    $(DRIVER_SOURCES) platform/pi4/hardware.c platform/pi4/startup.c \
    platform/pi4/sd.c platform/pi4/storage.c arch/armv8/task_port.c \
    arch/armv8/irq.c arch/armv8/trap.c arch/armv8/address_space.c \
    kernel/boot/main.c kernel/syscall/dispatch.c kernel/ui/render.c
$(patsubst %.c,$(BUILD_DIR)/%.o,$(STRICT_SOURCES)): CFLAGS += -Wextra -Werror
$(BUILD_DIR)/lib/memory.o: CFLAGS += -fno-tree-loop-distribute-patterns
$(BUILD_DIR)/arch/armv8/task_port.o: CFLAGS += -mno-outline-atomics

all: $(BUILD_DIR)/prod/kernel8.img

.PHONY: all clean FORCE
FORCE:

$(BUILD_DIR)/build-config: FORCE tools/build_config.py
	@$(PYTHON) tools/build_config.py --output "$@" --cc="$(CC)" --ld="$(LD)" --objcopy="$(OBJCOPY)" --cflags="$(BUILD_CONFIG_FLAGS)" --firmware="$(WIFI_FIRMWARE_C)"

ifeq ($(WIFI),1)
OBJECTS += $(BUILD_DIR)/drivers/wifi/assets.o
$(BUILD_DIR)/drivers/wifi/assets.o: $(WIFI_FIRMWARE_C)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
endif

$(OBJECTS) $(USER_OBJECTS): $(BUILD_DIR)/build-config Makefile mk/config.mk mk/sources.mk

# An undefined symbol here means the program reached for a kernel service.
$(BUILD_DIR)/user/terminal.elf: $(USER_OBJECTS) user/program.ld
	$(LD) -T user/program.ld $(USER_OBJECTS) -o $@

$(BUILD_DIR)/user/terminal.bin: $(BUILD_DIR)/user/terminal.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/kernel/boot/terminal_image.o: $(BUILD_DIR)/user/terminal.bin
$(BUILD_DIR)/kernel/boot/terminal_image.o: CFLAGS += -Wa,-I$(BUILD_DIR)/user

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/prod/kernel8.img: $(OBJECTS) arch/armv8/linker.ld
	$(LD) -T arch/armv8/linker.ld $(OBJECTS) -o $(BUILD_DIR)/kernel8.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel8.elf $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d) $(USER_OBJECTS:.o=.d)
