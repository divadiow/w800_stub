#!/usr/bin/env python3
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


TRACKED_FILES = (
    ".gitattributes",
    ".gitignore",
    "Makefile",
    "README.md",
    "W800_RawMem_Stub.bin",
    "W800_RawMem_Stub.img",
    "src/start.S",
    "src/stub.ld",
    "src/w800_libc.c",
    "src/w800_miniz.c",
    "src/w800_miniz.h",
    "src/w800_raw_stub.c",
    "third_party/miniz/LICENSE",
    "third_party/miniz/miniz.c",
    "third_party/miniz/miniz.h",
    "third_party/miniz/miniz_common.h",
    "third_party/miniz/miniz_export.h",
    "third_party/miniz/miniz_tdef.c",
    "third_party/miniz/miniz_tdef.h",
    "third_party/miniz/miniz_tinfl.c",
    "third_party/miniz/miniz_tinfl.h",
    "third_party/miniz/miniz_zip.h",
    "tools/make_build_manifest.py",
    "tools/make_w800_image.py",
    "tools/test_w800_deflate.c",
    "w800_custom_stub_probe.py",
)


def file_record(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Create a W800 stub build manifest")
    parser.add_argument("--status", default="compiled", help="Validation status recorded in the manifest")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    manifest = {
        "package": "w800_stub_v0.13",
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "status": args.status,
        "line_endings": "LF",
        "stub_load_address": "0x20004000",
        "compression": {
            "format": "raw DEFLATE",
            "library": "miniz",
            "revision": "77d0dce8627735138c51770d1799a1ef48f2117d",
            "TDEFL_LESS_MEMORY": 1,
        },
        "crc32": {
            "implementation": "W800 crypto peripheral with 4 KiB RAM staging",
            "fallback": "first-use self-test and polling timeout use software CRC32",
        },
        "command_protocols": {
            "rom_bootstrap": ["WinnerMicro 0x21 framing", "XMODEM image upload"],
            "obk": ["0x00", "0x04", "0x05", "0x07", "0x09", "0x8F", "0x90", "0x91", "0x92", "0x95", "0x96", "0x97", "0x98"],
            "unsupported": ["0x93", "0x94", "0x99"],
        },
        "files": {name: file_record(root / name) for name in TRACKED_FILES},
    }
    output = root / "build_manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
