# W800/W806 custom stub

RAM-loaded flash stub for WinnerMicro W800 and W806 devices. The running stub uses the common Easy Flasher `0xA5` custom-stub protocol; WinnerMicro framing is used only to enter mask-ROM download mode and upload the image.

## Commands

| Command | Operation |
|---|---|
| `0x00` | Synchronize |
| `0x04` | Erase an aligned flash range |
| `0x05` | Erase writable flash |
| `0x07` | Change baud |
| `0x8f` | Hardware-assisted WinnerMicro CRC32 |
| `0x90` | Read JEDEC flash ID |
| `0x91` / `0x92` | Raw XMODEM flash write/read |
| `0x95` | Read validated W800 MAC |
| `0x96` / `0x97` | Raw-DEFLATE XMODEM flash read/write |
| `0x98` | Read mapped memory through XMODEM |

Unknown commands, including KV `0x93`/`0x94`, SHA-256 `0x09`, and silicon eFuse `0x99`, return `TYPE_ERROR`. W806 has no RF and returns an error for the MAC command.

Baud rates from 115200 through 2500000 inclusive are accepted. Flash offsets below `0x2000` are protected from erase and write operations.

## Build

Use a C-SKY ABI-v2 toolchain such as `csky-elfabiv2-tools-x86_64-minilibc-20250328`, then run:

```sh
make clean all host-test TOOLCHAIN=/path/to/toolchain/bin
```

The build creates `W800_RawMem_Stub.img` and its deterministic gzip form `W800_RawMem_Stub.bin`. Generated images and manifests are intentionally not tracked.

Compressed transfers use vendored miniz with `TDEFL_LESS_MEMORY=1`; see `third_party/miniz/LICENSE`.

## Validation

The shared image has been exercised on 2 MiB W800 and 1 MiB W806 hardware. On W800, raw and compressed write/read/erase restoration passed at every Easy Flasher rate accepted by the stub: 115200, 230400, 460800, 921600, 1500000, and 2000000 baud. Values outside the accepted range, including 3000000, were rejected. The 2500000 ceiling is not hardware-tested.

Install pyserial and run the non-destructive protocol probe with:

```sh
python w800_custom_stub_probe.py --port COM27 --probe-only
```

Destructive scratch tests are opt-in. `--test-baud-flash-mutation` checks raw and compressed write/read/erase restoration at all supported Easy Flasher rates.
