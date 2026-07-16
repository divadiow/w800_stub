# Minimal W800 custom raw-memory RAM stub build.
# Set TOOLCHAIN to the directory containing csky-abiv2-elf-gcc.
TOOLCHAIN ?= /mnt/data/csky_toolchain/bin
CC := $(TOOLCHAIN)/csky-abiv2-elf-gcc
OBJCOPY := $(TOOLCHAIN)/csky-abiv2-elf-objcopy
OBJDUMP := $(TOOLCHAIN)/csky-abiv2-elf-objdump
PYTHON ?= python3

CFLAGS := -mcpu=ck804ef -mhard-float -Os -std=gnu99 -ffunction-sections -fdata-sections -fno-builtin -nostdlib -nodefaultlibs -nostartfiles
LDFLAGS := -mcpu=ck804ef -mhard-float -nostdlib -nodefaultlibs -nostartfiles -Wl,--gc-sections -Wl,-Tsrc/stub.ld -Wl,-Map=build/w800_raw_stub.map

all: W800_RawMem_Stub.bin

build:
	mkdir -p build

build/start.o: src/start.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_raw_stub.o: src/w800_raw_stub.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_raw_stub.elf: build/start.o build/w800_raw_stub.o src/stub.ld
	$(CC) $(LDFLAGS) build/start.o build/w800_raw_stub.o -o $@
	$(OBJDUMP) -d $@ > build/w800_raw_stub.asm

build/w800_raw_stub_code.bin: build/w800_raw_stub.elf
	$(OBJCOPY) -O binary $< $@

W800_RawMem_Stub.img W800_RawMem_Stub.bin: build/w800_raw_stub_code.bin tools/make_w800_image.py
	$(PYTHON) tools/make_w800_image.py build/w800_raw_stub_code.bin W800_RawMem_Stub.img W800_RawMem_Stub.bin

clean:
	rm -rf build W800_RawMem_Stub.img W800_RawMem_Stub.bin
