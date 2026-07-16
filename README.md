# W800 RAM flasher stub

This repository contains a RAM-loaded flasher stub for WinnerMicro W800 devices and host-side tools for building and testing it.

The current development baseline provides:

- WinnerMicro `0x21` command framing for baud changes, erase, version reporting, reset, flash identification, and raw mapped-memory reads.
- WinnerMicro pseudo-FLS XMODEM writes compatible with the existing W800 host workflow.
- Easy Flasher custom-stub `0xA5` command framing for synchronization, erase, read, CRC32, and XMODEM write operations.
- Raw reads from mapped QFLASH, mask ROM, and RAM.
- W800 image-header generation and deterministic gzip packaging.

The OBK `0x04` range erase, `0x05` writable-area erase, and `0x91` XMODEM write commands are enabled at or above flash offset `0x2000`. Native command `0x32` supports the sector and 64 KiB block erase shapes used by the existing W800 host. Erases and programmed pages are verified on the target. The first 8 KiB ROM-managed parameter area remains protected from mutation commands.

## Hardware validation

The v0.6 read/protocol milestone has been tested on a W800 at 115200 and 460800 baud. The validated operations are:

- WinnerMicro flash ID, version, baud change, reset, and CRC-protected raw reads.
- OBK synchronization, flash ID, flash CRC32, baud change, flash XMODEM upload, and absolute-memory XMODEM upload.
- A complete 2 MiB QFLASH capture in 512 chunks with every chunk passing its wire CRC32 check.
- Full 20 KiB mask-ROM and 8 KiB QFLASH parameter-area captures.
- A cross-sector flash write with a partial final page, readback verification, range erase, and erased-state verification.
- Rejection of unaligned erase requests and erase requests targeting the protected first 8 KiB.
- Native scratch-range pseudo-FLS write, exact readback, and native erase restoration at 460800 baud.
- Two complete 2 MiB destructive cycles using native `0x32` erase, including a full restore through a 2,041-block pseudo-FLS transfer and exact full-chip comparison. The verified backup for that cycle has SHA-256 `e7d506362deb4025e4a3987720d65c20dc9b597a949bea987d4bac8d6a620ded`.
- Embedded-stub upload, post-upload synchronization, and a 20 KiB mask-ROM read through the compiled Easy Flasher `WMFlasher` path. The ROM SHA-256 matched the independent probe capture: `9ed209e5bda554272de8410683f18ac76a849d68b02407a864447f3056680a89`.
- Two compiled Easy Flasher full-backup writes followed by independent 2 MiB reads. Outside the firmware-managed EasyFlash sector at `0x001DB000`, the post-boot images had zero differing bytes.

Compressed transfers and silicon eFuse access are not part of this milestone.

## Memory layout

- QFLASH mapping: `0x08000000`
- Mask ROM: `0x00000000` through `0x00004fff`
- Stub load address: `0x20004000`
- W800 RAM: `0x20000000` through `0x20047fff`

The QFLASH area at `0x08000000` contains ROM-managed key and parameter data. It is not described here as silicon eFuse or OTP, and custom-stub command `0x99` remains unsupported until a silicon eFuse payload contract is established.

## Build

Set `TOOLCHAIN` to the directory containing the C-SKY compiler tools and run:

```sh
make clean all
```

The build produces `W800_RawMem_Stub.img` and its deterministic gzip-compressed form, `W800_RawMem_Stub.bin`.

## Hardware probe

Install pyserial and run a short validation pass:

```sh
python -m pip install pyserial
python w800_custom_stub_probe.py --port COM27 --manual-reset --probe-only
```

The probe enters W800 download mode with the `AT+Z` and ESC sequence, uploads the stub with XMODEM-1K, and checks QFLASH, ROM, and RAM reads with per-chunk CRC32 validation.

Do not use erase or write experiments on hardware containing data that must be preserved.
