# W800 RAM flasher stub

This repository contains a RAM-loaded flasher stub for WinnerMicro W800 devices and host-side tools for building and testing it.

The current development baseline provides:

- WinnerMicro `0x21` command framing for baud changes, version reporting, reset, flash identification, and raw mapped-memory reads.
- Easy Flasher custom-stub `0xA5` command framing for synchronization and read-oriented operations.
- Raw reads from mapped QFLASH, mask ROM, and RAM.
- W800 image-header generation and deterministic gzip packaging.

The baseline is intentionally read-focused. Flash erase and write commands remain disabled until their protection rules and hardware behaviour are tested.

## Hardware validation

The v0.6 read/protocol milestone has been tested on a W800 at 115200 and 460800 baud. The validated operations are:

- WinnerMicro flash ID, version, baud change, reset, and CRC-protected raw reads.
- OBK synchronization, flash ID, flash CRC32, baud change, flash XMODEM upload, and absolute-memory XMODEM upload.
- A complete 2 MiB QFLASH capture in 512 chunks with every chunk passing its wire CRC32 check.
- Full 20 KiB mask-ROM and 8 KiB QFLASH parameter-area captures.

Erase and write support is not part of this milestone.

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
