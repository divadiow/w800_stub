# W800/W806 RAM flasher stub

This repository contains a RAM-loaded flasher stub for WinnerMicro W800 and W806 devices, plus host-side build and hardware-test tools.

The running stub accepts only the common Easy Flasher custom-stub `0xA5` protocol. WinnerMicro `0x21` framing is used by the host only while entering mask-ROM download mode and uploading the RAM image; it is not part of the running stub command surface.

## Command surface

Supported commands:

- `0x00`: synchronize.
- `0x04`: erase a sector-aligned flash range at or above offset `0x2000`.
- `0x05`: erase the writable flash area while preserving offsets `0x0000` through `0x1fff`.
- `0x07`: change baud to 115200, 230400, 460800, 921600, 1000000, 1250000, 1500000, or 2000000.
- `0x09`: return SHA-256 for a flash range.
- `0x8f`: return WinnerMicro-format CRC32 for a flash range, using the crypto peripheral when its first-use self-test passes.
- `0x90`: return the JEDEC flash ID.
- `0x91`: receive a flash write through XMODEM-CRC.
- `0x92`: send a flash range through XMODEM.
- `0x95`: return the validated six-byte W800 Wi-Fi MAC address.
- `0x96`: send a raw-DEFLATE-compressed flash range through XMODEM.
- `0x97`: receive a raw-DEFLATE-compressed flash write through XMODEM-CRC.
- `0x98`: send an absolute mapped-memory range through XMODEM.

KV commands `0x93` and `0x94` and silicon eFuse command `0x99` return `TYPE_ERROR`.

The MAC is read from the WinnerMicro factory-parameter block in QFLASH only after its magic and CRC32 are validated. W806 has no RF and returns an error for `0x95`; the stub does not manufacture a default MAC.

The public WinnerMicro SDK labels its flash-backed factory parameters as a virtual eFuse. This stub does not expose that ordinary flash data as silicon eFuse. No documented or SDK-backed silicon eFuse read contract has been established for W800/W806, so `0x99` remains unsupported.

## Hardware validation

The common-only image has been validated on a 2 MiB W800 on COM27:

- Mask-ROM entry, XMODEM-1K RAM upload, and post-upload synchronization.
- Common synchronization and JEDEC flash-size detection.
- Exact SHA-256 and CRC32 comparison against host calculations; SHA-256 was checked across 1, 55, 56, 63, 64, 65, 257, and 4096-byte ranges.
- W800 MAC comparison against both ROM command `0x38` and the validated factory block.
- Flash and absolute-memory XMODEM reads, including QFLASH, mask ROM, and RAM.
- Raw-DEFLATE compressed reads at levels 1, 2, 5, and 9 and compressed writes using Easy Flasher-compatible framing.
- 115200, 230400, 460800, 921600, 1000000, 1500000, and 2000000 baud, returning to 115200 after each test. The stub also accepts the SDK-defined 1250000 rate, but the COM27 USB-UART path did not preserve framing at that rate, so it is not hardware-verified here.
- KV and silicon eFuse commands returning unsupported status.

The same common-only image also passed a 4,387-byte cross-sector scratch write, exact readback, range erase, rejection of unaligned/protected erases, and complete 2 MiB uncompressed and compressed backup/writable-area erase/restore cycles. A separate 256 KiB non-`FF` stress pattern passed compressed write, compressed readback, and exact erase restoration. The first 8 KiB remained unchanged and the final full-chip image exactly matched the backup with SHA-256 `54915fb4f5ec1aeffdab79ed181a28837559a7f25b51c429b183573c954f0e6e`.

A 1 MiB W806 on COM49 reports JEDEC ID `85 60 14` and rejects Wi-Fi MAC access as expected for a no-RF part. The v0.7 shared image passed the full command, SHA boundary, compressed-read, and baud suites, a 4,387-byte compressed and uncompressed scratch write/read/erase cycle at offset `0x000c0000`, a 256 KiB non-`FF` compressed stress cycle, and a complete 1 MiB compressed backup/writable-area erase/restore cycle. The first 8 KiB remained unchanged and the final full-chip image exactly matched the backup with SHA-256 `cee91203ec86d44a3832e8879cfb77ae79f61366a98d34fc7a9646b81af9f4a2`.

The v0.8 image replaces the original compact DEFLATE implementation with miniz. On the same W800 flash contents at 921600 baud, a complete 2 MiB compressed read transferred 470,016 bytes in 12,253 ms and completed in 22,142 ms. The preceding implementation transferred 542,720 bytes in about 22,628 ms and completed in about 31,353 ms. A two-segment 708,400-byte W800 FLS compressed write completed in 23,931 ms with both segment hashes matching. The miniz image also passed a 256 KiB compressed write/read/restore stress cycle on the W806.

The v0.9 image uses switch-based command dispatch and adds the remaining useful SDK-defined baud rates. Its W800 command, write/read/erase, CRC32, resident-stub, and 1500000-baud paths have been hardware-verified on COM27.

The v0.10 image accelerates CRC32 with the W800 crypto peripheral. QFLASH is staged through the existing 4 KiB RAM command buffer because the crypto DMA does not read the CPU QFLASH mapping correctly. A first-use comparison against the software implementation disables hardware CRC automatically if the peripheral fails, and a polling timeout also falls back to software. On the COM27 W800, best command times fell from 104.204 ms to 51.775 ms for 64 KiB, from 1,353.584 ms to 507.777 ms for 1 MiB, and from 2,703.197 ms to 993.683 ms for 2 MiB. Unaligned and cross-buffer CRC ranges were checked against host calculations.

The v0.11 image uses 64 KiB block erases for complete aligned blocks and 4 KiB sector erases for range edges. A complete 2 MiB W800 writable-area erase took 1.357 seconds on COM27. The first 8 KiB remained unchanged, the rest of QFLASH was verified erased, and the pre-erase image was restored and verified exactly.

The v0.12 image applies the mixed block/sector erase before raw XMODEM writes and skips programming pages whose requested contents are entirely `0xFF`. Programmed pages retain immediate readback comparison. On the same 2 MiB image, containing 5,363 blank pages out of 8,192, the complete writable-range raw restore at 460800 baud fell from 84.631 seconds to 68.160 seconds. The restore took 48.237 seconds at 921600 baud. Both restored images passed exact full-chip verification.

The v0.13 image reduces the sender's ACK-response wait from approximately 10 seconds to approximately 2 seconds and allows 30 packet retries. Easy Flasher requests retransmission after one second on the W800 custom-stub read path, before the stub's autonomous retry can overlap a partially received packet. At 921600 baud, 15 consecutive cold-upload 2 MiB raw reads passed with matching device and host CRC32 values in 28,786 to 30,034 ms. Ten consecutive cold-upload compressed reads of the same flash contents also passed with matching CRC32 values in 11,691 to 11,804 ms, transferring 472,064 bytes per read. No 10-second retry band or truncated transfer occurred in either application-level series.

## Memory layout

- QFLASH mapping: `0x08000000`.
- Mask ROM: `0x00000000` through `0x00004fff`.
- Stub load address: `0x20004000`.
- RAM: `0x20000000` through `0x20047fff`.
- Linked stub data ends at `0x2003d5bc`, leaving 35,396 bytes below the reserved 8 KiB stack area.

## Build

Set `TOOLCHAIN` to the directory containing the C-SKY ABI-v2 compiler tools and run:

```sh
make clean all manifest
```

The build produces `W800_RawMem_Stub.img`, its deterministic gzip-compressed form `W800_RawMem_Stub.bin`, and `build_manifest.json`. Repository text files use LF line endings.

Compressed transfers use vendored miniz revision `77d0dce8627735138c51770d1799a1ef48f2117d`. The build defines `TDEFL_LESS_MEMORY=1` and the adapter rejects builds where that setting is disabled. The miniz license is in `third_party/miniz/LICENSE`.

Run `make host-test` on a host with zlib development files to check raw-DEFLATE encoder and decoder compatibility.

## Hardware probe

Install pyserial and run a non-destructive validation pass:

```sh
python -m pip install pyserial
python w800_custom_stub_probe.py --port COM27 --probe-only
```

The probe handles the W800 application reset/ESC path and the W806 secondary-downloader reset/ESC path, uploads the stub, checks the common command surface, and captures short QFLASH, ROM, and RAM samples. A silent W806 application requires the board's documented power-on sequence: unplug USB, hold BOOT, reconnect USB, then release BOOT. `--reset-esc-only` is available when a physical reset must be caught without sending `AT+Z`. Destructive tests are opt-in.
