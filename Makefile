# Minimal W800 custom raw-memory RAM stub build.
# TOOLCHAIN is the directory containing the compiler tools.
# CROSS_COMPILE is the executable prefix, including its trailing dash.
TOOLCHAIN ?= /mnt/data/csky_toolchain/bin
CROSS_COMPILE ?= csky-abiv2-elf-
CC := $(TOOLCHAIN)/$(CROSS_COMPILE)gcc
OBJCOPY := $(TOOLCHAIN)/$(CROSS_COMPILE)objcopy
OBJDUMP := $(TOOLCHAIN)/$(CROSS_COMPILE)objdump
PYTHON ?= python3
HOST_CC ?= gcc

CFLAGS := -mcpu=ck804ef -mhard-float -Os -std=gnu99 -ffunction-sections -fdata-sections -fno-builtin -nostdlib -nodefaultlibs -nostartfiles
MINIZ_FLAGS := -DTDEFL_LESS_MEMORY=1 -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -DNDEBUG
CFLAGS += -Isrc -Ithird_party/miniz $(MINIZ_FLAGS)
LDFLAGS := -mcpu=ck804ef -mhard-float -nostdlib -nodefaultlibs -nostartfiles -Wl,--gc-sections -Wl,-Tsrc/stub.ld -Wl,-Map=build/w800_raw_stub.map

all: W800_RawMem_Stub.bin

build:
	mkdir -p build

build/start.o: src/start.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_raw_stub.o: src/w800_raw_stub.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_miniz.o: src/w800_miniz.c src/w800_miniz.h third_party/miniz/miniz.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_libc.o: src/w800_libc.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/miniz_tdef.o: third_party/miniz/miniz_tdef.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/miniz_tinfl.o: third_party/miniz/miniz_tinfl.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/miniz.o: third_party/miniz/miniz.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/w800_raw_stub.elf: build/start.o build/w800_raw_stub.o build/w800_miniz.o build/w800_libc.o build/miniz.o build/miniz_tdef.o build/miniz_tinfl.o src/stub.ld
	$(CC) $(LDFLAGS) build/start.o build/w800_raw_stub.o build/w800_miniz.o build/w800_libc.o build/miniz.o build/miniz_tdef.o build/miniz_tinfl.o -o $@
	$(OBJDUMP) -d $@ > build/w800_raw_stub.asm

build/w800_raw_stub_code.bin: build/w800_raw_stub.elf
	$(OBJCOPY) -O binary $< $@

W800_RawMem_Stub.img W800_RawMem_Stub.bin: build/w800_raw_stub_code.bin tools/make_w800_image.py
	$(PYTHON) tools/make_w800_image.py build/w800_raw_stub_code.bin W800_RawMem_Stub.img W800_RawMem_Stub.bin

clean:
	rm -rf build W800_RawMem_Stub.img W800_RawMem_Stub.bin

host-test: | build
	$(HOST_CC) -O2 -std=c99 -Wall -Wextra -Isrc -Ithird_party/miniz $(MINIZ_FLAGS) tools/test_w800_deflate.c src/w800_miniz.c third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c -lz -o build/test_w800_deflate
	build/test_w800_deflate

.PHONY: all clean host-test
