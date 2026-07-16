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
    "src/w800_raw_stub.c",
    "tools/make_build_manifest.py",
    "tools/make_w800_image.py",
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
        "package": "w800_stub_v0.6",
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "status": args.status,
        "line_endings": "LF",
        "stub_load_address": "0x20004000",
        "command_protocols": {
            "winner_micro": ["0x31", "0x3C", "0x3E", "0x3F", "0x4A"],
            "obk": ["0x00", "0x04", "0x07", "0x8F", "0x90", "0x91", "0x92", "0x98"],
            "unsupported": ["0x05", "0x09", "0x96", "0x97", "0x99"],
        },
        "files": {name: file_record(root / name) for name in TRACKED_FILES},
    }
    output = root / "build_manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
