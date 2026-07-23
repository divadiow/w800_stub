#!/usr/bin/env python3
"""
W800/W806 common-protocol RAM-stub probe.

v0.7: validates common-protocol raw-DEFLATE transfers in both directions.

Requires: pyserial
Run example: py -3 w800_custom_stub_probe.py --port COM27 --manual-reset --probe-only
"""

from __future__ import annotations

import argparse
import binascii
import gzip
import hashlib
import json
from pathlib import Path
import struct
import sys
import time
import zlib
from typing import Dict, Iterable, List, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:
    serial = None  # type: ignore

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRCCHR = 0x43  # 'C'
SUB = 0x1A
PAD_FF = 0xFF

OBK_MAGIC = 0xA5
OBK_ACK_MAGIC = 0x5A
OBK_STATUS_SUCCESS = 0x00
OBK_STATUS_ERROR = 0x01
OBK_STATUS_ADDR_ERROR = 0x02
OBK_STATUS_TYPE_ERROR = 0x03

DEFAULT_READS = [
    # Known-good control first: QFLASH/key-parameter window.
    ("qflash_keyparam_08000000_00002000", 0x08000000, 0x2000, False),
    # This should now succeed with the custom stub, proving raw RAM read.
    ("custom_stub_vector_20004000", 0x20004000, 0x100, True),
    # Documented ROM candidate from W800 ROM material.
    ("rom_00000000_00005000", 0x00000000, 0x5000, False),
    ("ram_rom_stackheap_20000000_00000100", 0x20000000, 0x100, False),
    ("ram_mac_nc_20028000_00000100", 0x20028000, 0x100, False),
]

ALIAS_READ = ("rom_alias_candidate_1ff00000_00000100", 0x1FF00000, 0x100, False)


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE: used by W800 command frames."""
    crc = init & 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc16_xmodem(data: bytes, init: int = 0x0000) -> int:
    """CRC-16/XMODEM: used inside XMODEM-CRC packets."""
    crc = init & 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    """Python/zlib normal CRC32, kept for file analysis output."""
    return binascii.crc32(data) & 0xFFFFFFFF


def crc32_wm(data: bytes) -> int:
    """WinnerMicro reflected CRC32 with initial 0xffffffff and no final XOR."""
    return crc32(data) ^ 0xFFFFFFFF


def hexdump_prefix(data: bytes, n: int = 64) -> str:
    return data[:n].hex(" ")


def ascii_preview(data: bytes, n: int = 64) -> str:
    out = []
    for b in data[:n]:
        out.append(chr(b) if 32 <= b <= 126 else ".")
    return "".join(out)


def parse_int(text: str) -> int:
    text = text.strip().replace("_", "")
    return int(text, 0)


def load_stub_image(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw.startswith(b"\x1f\x8b"):
        img = gzip.decompress(raw)
    else:
        img = raw
    if len(img) < 64:
        raise ValueError(f"Stub image too small after decompression: {len(img)} bytes")
    magic, img_attr, img_addr, img_len = struct.unpack_from("<IIII", img, 0)
    if magic != 0xA0FFFF9F:
        raise ValueError(f"Stub does not start with W800 image magic 0xA0FFFF9F; got 0x{magic:08x}")
    if img_addr != 0x20004000:
        print(f"WARN: stub header img_addr is 0x{img_addr:08x}, not 0x20004000")
    if img_len + 64 > len(img):
        print(f"WARN: header img_len=0x{img_len:x} extends beyond image length {len(img)}")
    return img


def w800_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<I", cmd) + payload
    # WinnerMicro command framing observed in WMFlasher.cs:
    # 0x21, little-endian length including CRC16+body, little-endian CRC16/CCITT-FALSE, body.
    return b"\x21" + struct.pack("<H", len(body) + 2) + struct.pack("<H", crc16_ccitt_false(body)) + body


def obk_frame(command: int, payload: bytes = b"") -> bytes:
    frame = bytearray((OBK_MAGIC, command & 0xFF, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF))
    frame.extend(payload)
    frame.append(sum(frame) & 0xFF)
    return bytes(frame)


class SerialTimeout(RuntimeError):
    pass


class W800Probe:
    def __init__(self, port: str, baud: int, timeout: float, verbose: bool = False):
        if serial is None:
            raise RuntimeError("pyserial is not installed. Install with: py -m pip install pyserial")
        self.ser = serial.Serial(
            port=port,
            baudrate=baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=timeout,
            write_timeout=timeout,
        )
        try:
            # Matches the C# flasher: large enough for 4096-byte read replies plus CRC.
            self.ser.set_buffer_size(rx_size=65536, tx_size=65536)  # type: ignore[attr-defined]
        except Exception:
            pass
        self.verbose = verbose
        self.flash_size = 0
        self.current_read_metas: List[Dict[str, object]] = []

    def close(self) -> None:
        self.ser.close()

    def set_timeout(self, timeout: float) -> None:
        self.ser.timeout = timeout
        self.ser.write_timeout = max(timeout, 1.0)

    def drain(self, seconds: float = 0.2, *, max_total: Optional[float] = None, max_bytes: int = 65536, extend_on_data: bool = True) -> bytes:
        """Drain serial RX without allowing continuous prompt bytes to hang forever.

        Earlier probe versions extended the quiet timer every time data arrived.
        That is fine for finite replies, but W800 ROM mode can emit an endless
        stream of 'C' prompt bytes. max_total is therefore a hard cap.
        """
        start = time.time()
        end = start + seconds
        hard_end = start + (max_total if max_total is not None else max(seconds, 2.0))
        out = bytearray()
        while time.time() < end and time.time() < hard_end and len(out) < max_bytes:
            wanted = min(4096, max_bytes - len(out))
            if wanted <= 0:
                break
            b = self.ser.read(wanted)
            if b:
                out.extend(b)
                if extend_on_data:
                    end = min(time.time() + seconds, hard_end)
        return bytes(out)

    def reset_buffers(self) -> None:
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        try:
            self.ser.reset_output_buffer()
        except Exception:
            pass

    def set_lines(self, *, rts: Optional[bool] = None, dtr: Optional[bool] = None) -> None:
        if rts is not None:
            try:
                self.ser.rts = rts
            except Exception as e:
                print(f"WARN: failed to set RTS={rts}: {e}")
        if dtr is not None:
            try:
                self.ser.dtr = dtr
            except Exception as e:
                print(f"WARN: failed to set DTR={dtr}: {e}")

    def wait_for_any(self, wanted: Iterable[int], timeout: float, label: str) -> int:
        wanted_set = set(wanted)
        end = time.time() + timeout
        seen = bytearray()
        old_timeout = self.ser.timeout
        self.ser.timeout = min(float(old_timeout or timeout), 0.1)
        try:
            while time.time() < end:
                b = self.ser.read(1)
                if not b:
                    continue
                c = b[0]
                seen.append(c)
                if self.verbose:
                    printable = chr(c) if 32 <= c <= 126 else "."
                    print(f"[rx] 0x{c:02x} {printable}")
                if c in wanted_set:
                    return c
        finally:
            self.ser.timeout = old_timeout
        raise SerialTimeout(f"Timed out waiting for {label}. Last bytes: {seen[-96:].hex(' ')} | {ascii_preview(seen[-96:])}")

    def read_exact(self, n: int, timeout: float, label: str) -> bytes:
        end = time.time() + timeout
        out = bytearray()
        old_timeout = self.ser.timeout
        self.ser.timeout = 0.05
        try:
            while len(out) < n and time.time() < end:
                chunk = self.ser.read(n - len(out))
                if chunk:
                    out.extend(chunk)
                    if self.verbose:
                        print(f"[rx] {len(chunk)} bytes ({len(out)}/{n})")
        finally:
            self.ser.timeout = old_timeout
        if len(out) != n:
            raise SerialTimeout(
                f"Timed out reading {label}: expected {n}, got {len(out)}. "
                f"Tail: {bytes(out[-96:]).hex(' ')} | {ascii_preview(bytes(out[-96:]))}"
            )
        return bytes(out)

    def wait_for_cccc(self, timeout: float, *, preserve_count_on_timeout: bool, label: str) -> bool:
        count = 0
        end = time.time() + timeout
        seen = bytearray()
        old_timeout = self.ser.timeout
        self.ser.timeout = 0.01 if preserve_count_on_timeout else 0.1
        try:
            while time.time() < end:
                b = self.ser.read(1)
                if not b:
                    if not preserve_count_on_timeout:
                        count = 0
                    continue
                c = b[0]
                seen.append(c)
                if self.verbose:
                    printable = chr(c) if 32 <= c <= 126 else "."
                    print(f"[sync-rx] 0x{c:02x} {printable} count={count}")
                if c == CRCCHR:
                    count += 1
                    if count > 3:
                        print(f"{label}: CCCC sync success")
                        return True
                else:
                    count = 0
        finally:
            self.ser.timeout = old_timeout
        if seen:
            print(f"{label}: no CCCC. Last RX: {seen[-96:].hex(' ')} | {ascii_preview(seen[-96:])}")
        else:
            print(f"{label}: no CCCC and no RX bytes")
        return False

    def sync_w800_download_mode(self, entry_timeout: float = 60.0, use_atz_esc: bool = True,
                                reset_esc_only: bool = False) -> None:
        # This intentionally follows BK7231Flasher's W800 path rather than the generic CCCC wait.
        self.set_lines(rts=False, dtr=False)
        self.reset_buffers()

        reset_from_downloader = False

        if self.wait_for_cccc(1.5, preserve_count_on_timeout=True, label="initial W800 prompt check"):
            self.reset_buffers()
            version = self.execute_command(0x3E, expected_len=3, timeout=1.0)
            print(f"Initial downloader version: {version!r}")
            if version == b"R:8":
                self.reset_buffers()
                return
            if version == b"R:W":
                self.reset_buffers()
                self.ser.write(w800_frame(0x3F))
                self.ser.flush()
                reset_ack = self.read_exact(1, 1.0, "W806 downloader reset acknowledgement")
                if reset_ack != b"C":
                    raise RuntimeError(f"W806 downloader reset returned {reset_ack!r}")
                reset_from_downloader = True
            else:
                raise RuntimeError(f"Unknown W800/W806 downloader version {version!r}")

        if not use_atz_esc and not reset_esc_only:
            raise SerialTimeout("No CCCC prompt and --no-atz-esc was set")

        if reset_from_downloader:
            print("W806 secondary downloader detected; catching mask ROM with ESC after reset...")
        elif reset_esc_only:
            print("Waiting for a physical reset while sending ESC...")
        else:
            print("W800 sync timeout, sending AT+Z/ESC bootloader entry sequence...")
            self.set_lines(rts=True)
            time.sleep(0.05)
            self.ser.write(b"AT+Z\r\n")
            self.ser.flush()
            self.set_lines(rts=False)

        esc_burst = b"\x1b\x1b\x1b"
        end = time.time() + entry_timeout
        count = 0
        seen = bytearray()
        old_timeout = self.ser.timeout
        self.ser.timeout = 0.01
        try:
            while time.time() < end:
                self.ser.write(esc_burst)
                self.ser.flush()
                local_end = time.time() + 0.01
                while time.time() < local_end:
                    b = self.ser.read(1)
                    if not b:
                        continue
                    c = b[0]
                    seen.append(c)
                    if self.verbose:
                        printable = chr(c) if 32 <= c <= 126 else "."
                        print(f"[entry-rx] 0x{c:02x} {printable} count={count}")
                    if c == CRCCHR:
                        count += 1
                        if count > 3:
                            print("AT+Z/ESC entry: CCCC sync success")
                            self.reset_buffers()
                            return
                    else:
                        count = 0
                time.sleep(0.01)
        finally:
            self.ser.timeout = old_timeout

        tail = bytes(seen[-128:])
        entry_method = "physical reset/ESC" if reset_esc_only else "AT+Z/ESC"
        raise SerialTimeout(
            f"W800/W806 sync failed: no CCCC download prompt after {entry_method}. "
            f"Tail RX: {tail.hex(' ')} | {ascii_preview(tail)}"
        )

    def sync_after_stub_upload(self, timeout: float = 20.0) -> None:
        # WMFlasher.cs calls Sync() after XMODEM upload. Keep this separate from initial AT+Z/ESC entry.
        self.reset_buffers()
        if not self.wait_for_cccc(timeout, preserve_count_on_timeout=True, label="post-stub prompt"):
            raise SerialTimeout("Stub upload completed but no post-stub CCCC prompt was received")
        self.reset_buffers()

    def execute_command(self, cmd: int, payload: bytes = b"", expected_len: int = 0, timeout: float = 1.0) -> bytes:
        frame = w800_frame(cmd, payload)
        self.reset_buffers()
        self.ser.write(frame)
        self.ser.flush()
        if expected_len <= 0:
            time.sleep(timeout)
            return self.drain(0.05)
        return self.read_exact(expected_len, timeout, f"command 0x{cmd:02x} reply")

    def execute_obk_command(self, command: int, payload: bytes = b"", timeout: float = 2.0) -> Tuple[bytes, int]:
        self.reset_buffers()
        self.ser.write(obk_frame(command, payload))
        self.ser.flush()
        header = self.read_exact(4, timeout, f"OBK command 0x{command:02x} header")
        if header[0] != OBK_ACK_MAGIC:
            raise RuntimeError(f"OBK command 0x{command:02x} returned bad magic 0x{header[0]:02x}")
        if header[1] != (command & 0xFF):
            raise RuntimeError(f"OBK command 0x{command:02x} returned type 0x{header[1]:02x}")
        data_len = header[2] | (header[3] << 8)
        tail = self.read_exact(data_len + 2, timeout, f"OBK command 0x{command:02x} payload")
        reply = header + tail
        if (sum(reply[:-1]) & 0xFF) != reply[-1]:
            raise RuntimeError(f"OBK command 0x{command:02x} reply checksum mismatch")
        return tail[:data_len], tail[-2]

    def read_obk_memory(self, addr: int, size: int, chunk_size: int, timeout: float) -> bytes:
        output = bytearray()
        self.current_read_metas = []
        while len(output) < size:
            chunk = min(chunk_size, size - len(output))
            current_addr = addr + len(output)
            _, status = self.execute_obk_command(0x98, struct.pack("<II", current_addr, chunk), timeout=timeout)
            if status != OBK_STATUS_SUCCESS:
                raise RuntimeError(f"OBK raw read at 0x{current_addr:08x} failed with status {status}")
            data = self.xmodem_receive(chunk, timeout=max(timeout, 10.0))
            output.extend(data)
            self.current_read_metas.append({
                "addr": f"0x{current_addr:08x}",
                "size": chunk,
                "protocol": "obk_0x98_xmodem",
                "sha256": hashlib.sha256(data).hexdigest(),
            })
        return bytes(output)

    def xmodem_receive(self, expected_len: Optional[int], timeout: float = 10.0) -> bytes:
        output = bytearray()
        expected_block = 1
        old_timeout = self.ser.timeout
        self.ser.timeout = min(timeout, 1.0)
        try:
            self.ser.write(bytes((CRCCHR,)))
            self.ser.flush()
            deadline = time.time() + timeout
            while time.time() < deadline:
                first = self.ser.read(1)
                if not first:
                    continue
                marker = first[0]
                if marker == EOT:
                    self.ser.write(bytes((ACK,)))
                    self.ser.flush()
                    if expected_len is not None and len(output) < expected_len:
                        raise RuntimeError(f"XMODEM ended after {len(output)} bytes, expected {expected_len}")
                    return bytes(output if expected_len is None else output[:expected_len])
                if marker == CAN:
                    raise RuntimeError("Target cancelled XMODEM transfer")
                if marker not in (SOH, STX):
                    continue
                block_size = 128 if marker == SOH else 1024
                packet = self.read_exact(block_size + 4, timeout, "XMODEM packet")
                block_no = packet[0]
                block_inv = packet[1]
                data = packet[2:2 + block_size]
                received_crc = struct.unpack(">H", packet[-2:])[0]
                if block_inv != (0xFF - block_no) or received_crc != crc16_xmodem(data):
                    self.ser.write(bytes((NAK,)))
                    self.ser.flush()
                    continue
                if block_no == expected_block:
                    output.extend(data)
                    expected_block = (expected_block + 1) & 0xFF
                elif block_no != ((expected_block - 1) & 0xFF):
                    self.ser.write(bytes((CAN, CAN)))
                    self.ser.flush()
                    raise RuntimeError(f"Unexpected XMODEM block {block_no}, expected {expected_block}")
                self.ser.write(bytes((ACK,)))
                self.ser.flush()
                deadline = time.time() + timeout
        finally:
            self.ser.timeout = old_timeout
        raise SerialTimeout(f"Timed out receiving XMODEM data after {len(output)} bytes")

    def test_obk_protocol(self) -> Dict[str, object]:
        results: Dict[str, object] = {}
        data, status = self.execute_obk_command(0x00)
        if data or status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK sync failed with status {status} and {len(data)} data bytes")
        results["sync"] = "ok"

        flash_id, status = self.execute_obk_command(0x90)
        if status != OBK_STATUS_SUCCESS or len(flash_id) != 4:
            raise RuntimeError(f"OBK flash ID failed with status {status} and {len(flash_id)} data bytes")
        self.flash_size = 1 << flash_id[2]
        results["flash_id_hex"] = flash_id.hex(" ")
        results["flash_size"] = self.flash_size

        flash_control = self.read_obk_memory(0x08000000, 0x3000, 0x1000, 3.0)
        crc_results: Dict[str, str] = {}
        for crc_off, crc_len in ((1, 1), (3, 4093), (3, 4096), (3, 4097), (7, 8193), (0, len(flash_control))):
            crc_reply, status = self.execute_obk_command(0x8F, struct.pack("<II", crc_off, crc_len), timeout=3.0)
            if status != OBK_STATUS_SUCCESS or len(crc_reply) != 4:
                raise RuntimeError(f"OBK flash CRC32 0x{crc_off:x}+0x{crc_len:x} failed with status {status} and {len(crc_reply)} data bytes")
            target_crc = struct.unpack("<I", crc_reply)[0]
            expected_crc = crc32_wm(flash_control[crc_off:crc_off + crc_len])
            if target_crc != expected_crc:
                raise RuntimeError(f"OBK flash CRC32 0x{crc_off:x}+0x{crc_len:x} mismatch: target=0x{target_crc:08x}, expected=0x{expected_crc:08x}")
            crc_results[f"0x{crc_off:x}+0x{crc_len:x}"] = f"0x{target_crc:08x}"
        results["flash_crc32"] = crc_results

        mac, status = self.execute_obk_command(0x95)
        if flash_id[2] == 0x14:
            if mac or status != OBK_STATUS_ERROR:
                raise RuntimeError(f"OBK W806 MAC command returned status {status} and {len(mac)} data bytes")
            results["mac"] = "unavailable_no_rf"
        else:
            if status != OBK_STATUS_SUCCESS or len(mac) != 6:
                raise RuntimeError(f"OBK MAC command failed with status {status} and {len(mac)} data bytes")
            if mac != flash_control[8:14]:
                raise RuntimeError(f"OBK MAC {mac.hex(':')} differs from validated factory data {flash_control[8:14].hex(':')}")
            results["mac"] = mac.hex(":")

        _, status = self.execute_obk_command(0x92, struct.pack("<II", 0, len(flash_control)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK flash XMODEM read command failed with status {status}")
        flash_xmodem = self.xmodem_receive(len(flash_control))
        if flash_xmodem != flash_control:
            raise RuntimeError("OBK flash XMODEM data differs from the common raw-memory control read")
        results["flash_xmodem_read"] = "ok"

        compressed_reads: Dict[str, object] = {}
        for compressed_off, compressed_len, level in ((0, 1, 1), (1, 2, 2), (3, 257, 5), (0, 4096, 9)):
            payload = struct.pack("<II", compressed_off, compressed_len) + bytes((level,))
            _, status = self.execute_obk_command(0x96, payload)
            if status != OBK_STATUS_SUCCESS:
                raise RuntimeError(f"OBK compressed read 0x{compressed_off:x}+0x{compressed_len:x} failed with status {status}")
            compressed = self.xmodem_receive(None)
            try:
                restored = zlib.decompress(compressed, wbits=-15)
            except zlib.error as exc:
                raise RuntimeError(f"OBK compressed read returned invalid raw DEFLATE: {exc}") from exc
            expected = flash_control[compressed_off:compressed_off + compressed_len]
            if restored != expected:
                raise RuntimeError(f"OBK compressed read 0x{compressed_off:x}+0x{compressed_len:x} data mismatch")
            compressed_reads[f"0x{compressed_off:x}+0x{compressed_len:x}@{level}"] = {
                "xmodem_bytes": len(compressed),
            }
        results["compressed_flash_read"] = compressed_reads

        rom_xmodem = self.read_obk_memory(0x00000000, 0x100, 0x100, 3.0)
        if rom_xmodem[:8] != b"\x0e\x00\x00\x00\x0e\x00\x00\x00":
            raise RuntimeError("OBK raw XMODEM ROM read did not return the expected W800/W806 vector prefix")
        results["raw_xmodem_read"] = "ok"

        tested_bauds = []
        for baud in (230400, 460800, 921600, 1000000, 1500000, 2000000):
            self.change_obk_baud(baud)
            tested_bauds.append(baud)
            self.change_obk_baud(115200)
        results["bauds"] = [115200] + tested_bauds

        for baud in (115199, 2500001, 3000000):
            data, status = self.execute_obk_command(0x07, struct.pack("<I", baud))
            if data or status != OBK_STATUS_TYPE_ERROR:
                raise RuntimeError(f"OBK unsupported baud {baud} returned status {status} and {len(data)} data bytes")
        results["baud_limits"] = "115200..2500000"

        for command in (0x09, 0x93, 0x94):
            data, status = self.execute_obk_command(command)
            if data or status != OBK_STATUS_TYPE_ERROR:
                raise RuntimeError(f"OBK unsupported command 0x{command:02x} returned status {status} and {len(data)} data bytes")
        results["sha256_command"] = "unsupported"
        results["kv_commands"] = "unsupported"

        data, status = self.execute_obk_command(0x99)
        if data or status != OBK_STATUS_TYPE_ERROR:
            raise RuntimeError(f"OBK unsupported eFuse command returned status {status} and {len(data)} data bytes")
        results["efuse_command"] = "unsupported"
        return results

    def benchmark_crc32(self) -> Dict[str, object]:
        if not self.flash_size:
            raise RuntimeError("CRC32 benchmark requires a successful flash-ID probe")
        results: Dict[str, object] = {}
        for size, repeats in ((0x1000, 5), (0x10000, 5), (0x100000, 3), (self.flash_size, 3)):
            if size > self.flash_size or f"0x{size:x}" in results:
                continue
            samples = []
            values = []
            for _ in range(repeats):
                started = time.perf_counter()
                reply, status = self.execute_obk_command(0x8F, struct.pack("<II", 0, size), timeout=30.0)
                samples.append((time.perf_counter() - started) * 1000.0)
                if status != OBK_STATUS_SUCCESS or len(reply) != 4:
                    raise RuntimeError(f"CRC32 benchmark 0x{size:x} failed with status {status}")
                values.append(struct.unpack("<I", reply)[0])
            if len(set(values)) != 1:
                raise RuntimeError(f"CRC32 benchmark 0x{size:x} returned inconsistent values")
            result = {
                "bytes": size,
                "repeats": repeats,
                "crc32": f"0x{values[0]:08x}",
                "samples_ms": [round(sample, 3) for sample in samples],
                "best_ms": round(min(samples), 3),
                "average_ms": round(sum(samples) / len(samples), 3),
            }
            results[f"0x{size:x}"] = result
            print(f"CRC32 0x{size:x}: best {result['best_ms']:.3f} ms, average {result['average_ms']:.3f} ms")
        return results

    def test_obk_flash_mutation(self, scratch_offset: int) -> Dict[str, object]:
        sector_size = 0x1000
        test_length = 0x1123
        erase_length = ((test_length + sector_size - 1) // sector_size) * sector_size
        if scratch_offset < 0x2000 or (scratch_offset & (sector_size - 1)) != 0:
            raise ValueError("Scratch offset must be sector-aligned and at or above 0x2000")

        original = self.read_obk_memory(0x08000000 + scratch_offset, erase_length, sector_size, 5.0)
        if any(b != 0xFF for b in original):
            raise RuntimeError(f"Refusing destructive test: scratch range 0x{scratch_offset:08x}+0x{erase_length:x} is not erased")

        pattern = bytes((((i * 73) ^ (i >> 3) ^ 0xA5) & 0xFF) for i in range(test_length))
        _, status = self.execute_obk_command(0x91, struct.pack("<II", scratch_offset, len(pattern)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK flash write command failed with status {status}")
        self.xmodem_send(pattern, initial_wait=5.0, block_size=1024)
        written = self.read_obk_memory(0x08000000 + scratch_offset, erase_length, sector_size, 5.0)
        if written[:test_length] != pattern or any(b != 0xFF for b in written[test_length:]):
            raise RuntimeError("OBK flash write verification failed")

        _, status = self.execute_obk_command(0x04, struct.pack("<II", scratch_offset, erase_length), timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK sector erase command failed with status {status}")
        erased = self.read_obk_memory(0x08000000 + scratch_offset, erase_length, sector_size, 5.0)
        if erased != original:
            raise RuntimeError("OBK sector erase did not restore the original erased contents")

        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed_pattern = compressor.compress(pattern) + compressor.flush()
        _, status = self.execute_obk_command(0x97, struct.pack("<II", scratch_offset, len(pattern)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK compressed flash write command failed with status {status}")
        self.xmodem_send(compressed_pattern, initial_wait=5.0, block_size=1024, response_timeout=15.0)

        payload = struct.pack("<II", scratch_offset, len(pattern)) + bytes((5,))
        _, status = self.execute_obk_command(0x96, payload)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK compressed flash read command failed with status {status}")
        compressed_readback = self.xmodem_receive(None)
        try:
            compressed_restored = zlib.decompress(compressed_readback, wbits=-15)
        except zlib.error as exc:
            raise RuntimeError(f"OBK compressed flash read returned invalid raw DEFLATE: {exc}") from exc
        if compressed_restored != pattern:
            raise RuntimeError("OBK compressed flash write/read verification failed")

        _, status = self.execute_obk_command(0x04, struct.pack("<II", scratch_offset, erase_length), timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK post-compression sector erase command failed with status {status}")
        erased = self.read_obk_memory(0x08000000 + scratch_offset, erase_length, sector_size, 5.0)
        if erased != original:
            raise RuntimeError("OBK post-compression erase did not restore the original erased contents")

        _, status = self.execute_obk_command(0x04, struct.pack("<II", scratch_offset + 1, sector_size), timeout=2.0)
        if status != OBK_STATUS_ADDR_ERROR:
            raise RuntimeError(f"Unaligned OBK erase returned status {status}, expected {OBK_STATUS_ADDR_ERROR}")

        _, status = self.execute_obk_command(0x04, struct.pack("<II", 0, sector_size), timeout=2.0)
        if status != OBK_STATUS_ADDR_ERROR:
            raise RuntimeError(f"Protected-range OBK erase returned status {status}, expected {OBK_STATUS_ADDR_ERROR}")

        return {
            "scratch_offset": f"0x{scratch_offset:08x}",
            "bytes_written": len(pattern),
            "bytes_erased": erase_length,
            "write_sha256": hashlib.sha256(pattern).hexdigest(),
            "readback": "ok",
            "erase_restore": "ok",
            "compressed_write_bytes": len(compressed_pattern),
            "compressed_write_readback": "ok",
            "compressed_read": "ok",
            "unaligned_erase_rejected": "ok",
            "protected_erase_rejected": "ok",
        }

    def test_obk_compression_stress(self, scratch_offset: int, size: int) -> Dict[str, object]:
        sector_size = 0x1000
        if (scratch_offset < 0x2000 or (scratch_offset & (sector_size - 1)) or
                size <= 0 or (size & (sector_size - 1))):
            raise ValueError("Compression stress range must be positive, sector-aligned, and at or above 0x2000")
        if scratch_offset + size > self.flash_size:
            raise ValueError("Compression stress range exceeds detected flash size")
        original = self.read_obk_memory(0x08000000 + scratch_offset, size, sector_size, 10.0)
        if any(value != 0xFF for value in original):
            raise RuntimeError("Refusing compression stress test because the requested range is not erased")

        block = bytes((((i * 73) ^ (i >> 3) ^ 0xA5) & 0xFF) for i in range(sector_size))
        pattern = (block * ((size + len(block) - 1) // len(block)))[:size]
        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed = compressor.compress(pattern) + compressor.flush()

        self.change_obk_baud(460800)
        _, status = self.execute_obk_command(0x97, struct.pack("<II", scratch_offset, size), timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compression stress write command failed with status {status}")
        self.xmodem_send(compressed, initial_wait=10.0, block_size=1024, response_timeout=15.0)

        read_payload = struct.pack("<II", scratch_offset, size) + bytes((5,))
        _, status = self.execute_obk_command(0x96, read_payload, timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compression stress read command failed with status {status}")
        compressed_readback = self.xmodem_receive(None, timeout=60.0)
        restored = zlib.decompress(compressed_readback, wbits=-15)
        if restored != pattern:
            raise RuntimeError("Compression stress readback differs from the written pattern")

        _, status = self.execute_obk_command(0x04, struct.pack("<II", scratch_offset, size), timeout=90.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compression stress cleanup erase failed with status {status}")
        erased = self.read_obk_memory(0x08000000 + scratch_offset, size, sector_size, 10.0)
        if erased != original:
            raise RuntimeError("Compression stress cleanup did not restore the erased range")
        self.change_obk_baud(115200)
        return {
            "scratch_offset": f"0x{scratch_offset:08x}",
            "size": size,
            "host_compressed_bytes": len(compressed),
            "write_readback": "exact",
            "erase_restore": "exact",
        }

    def change_obk_baud(self, baud: int) -> None:
        print(f"Changing OBK baud to {baud}...")
        _, status = self.execute_obk_command(0x07, struct.pack("<I", baud))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK baud change to {baud} failed with status {status}")
        self.ser.baudrate = baud
        time.sleep(0.02)
        _, status = self.execute_obk_command(0x00)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK sync failed at {baud} baud")

    def test_full_chip_cycle(self, backup_path: Path, transfer_baud: int = 460800) -> Dict[str, object]:
        flash_size = self.flash_size
        if flash_size == 0:
            raise RuntimeError("Flash size is unknown; run the common protocol test first")
        protected_size = 0x2000
        self.change_obk_baud(transfer_baud)
        print("Backing up full QFLASH through OBK XMODEM...")
        _, status = self.execute_obk_command(0x92, struct.pack("<II", 0, flash_size))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Full-chip backup command failed with status {status}")
        backup = self.xmodem_receive(flash_size, timeout=30.0)
        backup_path.write_bytes(backup)
        backup_sha256 = hashlib.sha256(backup).hexdigest()

        print("Erasing writable QFLASH while preserving the first 8 KiB...")
        erase_started = time.perf_counter()
        _, status = self.execute_obk_command(0x05, timeout=90.0)
        erase_seconds = time.perf_counter() - erase_started
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK chip erase failed with status {status}")
        print(f"Writable QFLASH erase took {erase_seconds:.3f} seconds.")

        protected = self.read_obk_memory(0x08000000, protected_size, protected_size, 5.0)
        if protected != backup[:protected_size]:
            raise RuntimeError("Protected first 8 KiB changed during chip erase")
        _, status = self.execute_obk_command(0x92, struct.pack("<II", protected_size, flash_size - protected_size))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Erased-range read command failed with status {status}")
        erased = self.xmodem_receive(flash_size - protected_size, timeout=30.0)
        if any(b != 0xFF for b in erased):
            raise RuntimeError("Chip erase left programmed bytes in the writable flash range")

        payload = backup[protected_size:]
        print("Restoring the full writable QFLASH range through OBK XMODEM...")
        _, status = self.execute_obk_command(0x91, struct.pack("<II", protected_size, len(payload)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Full-chip restore command failed with status {status}")
        write_started = time.perf_counter()
        self.xmodem_send(payload, initial_wait=10.0, block_size=1024)
        write_seconds = time.perf_counter() - write_started
        print(f"Writable QFLASH restore took {write_seconds:.3f} seconds.")

        _, status = self.execute_obk_command(0x92, struct.pack("<II", 0, flash_size))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Full-chip verification read command failed with status {status}")
        restored = self.xmodem_receive(flash_size, timeout=30.0)
        if restored != backup:
            raise RuntimeError("Restored full-chip image differs from the pre-erase backup")

        self.change_obk_baud(115200)
        return {
            "flash_size": flash_size,
            "protected_size": protected_size,
            "transfer_baud": transfer_baud,
            "backup_file": str(backup_path),
            "backup_sha256": backup_sha256,
            "erase_protocol": "obk_0x05",
            "write_protocol": "obk_0x91",
            "chip_erase": "ok",
            "erase_seconds": erase_seconds,
            "write_seconds": write_seconds,
            "protected_range_preserved": "ok",
            "erased_range_verified": "ok",
            "full_restore_verified": "ok",
        }

    def test_compressed_full_chip_cycle(self, backup_path: Path) -> Dict[str, object]:
        flash_size = self.flash_size
        if flash_size == 0:
            raise RuntimeError("Flash size is unknown; run the common protocol test first")
        protected_size = 0x2000
        self.change_obk_baud(460800)

        print("Backing up full QFLASH through compressed OBK XMODEM...")
        read_payload = struct.pack("<II", 0, flash_size) + bytes((5,))
        _, status = self.execute_obk_command(0x96, read_payload, timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compressed full-chip backup command failed with status {status}")
        compressed_backup = self.xmodem_receive(None, timeout=60.0)
        try:
            backup = zlib.decompress(compressed_backup, wbits=-15)
        except zlib.error as exc:
            raise RuntimeError(f"Compressed full-chip backup is invalid raw DEFLATE: {exc}") from exc
        if len(backup) != flash_size:
            raise RuntimeError(f"Compressed full-chip backup returned {len(backup)} bytes, expected {flash_size}")
        backup_path.write_bytes(backup)
        backup_sha256 = hashlib.sha256(backup).hexdigest()

        print("Erasing writable QFLASH while preserving the first 8 KiB...")
        _, status = self.execute_obk_command(0x05, timeout=90.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK chip erase failed with status {status}")
        protected = self.read_obk_memory(0x08000000, protected_size, protected_size, 5.0)
        if protected != backup[:protected_size]:
            raise RuntimeError("Protected first 8 KiB changed during compressed full-chip cycle")

        writable = backup[protected_size:]
        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed_writable = compressor.compress(writable) + compressor.flush()
        print(f"Restoring {len(writable)} writable bytes from {len(compressed_writable)} compressed bytes...")
        _, status = self.execute_obk_command(0x97, struct.pack("<II", protected_size, len(writable)), timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compressed full-chip restore command failed with status {status}")
        self.xmodem_send(compressed_writable, initial_wait=10.0, block_size=1024, response_timeout=15.0)

        print("Verifying restored QFLASH through compressed OBK XMODEM...")
        _, status = self.execute_obk_command(0x96, read_payload, timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compressed full-chip verification command failed with status {status}")
        compressed_final = self.xmodem_receive(None, timeout=60.0)
        try:
            restored = zlib.decompress(compressed_final, wbits=-15)
        except zlib.error as exc:
            raise RuntimeError(f"Compressed full-chip verification is invalid raw DEFLATE: {exc}") from exc
        if restored != backup:
            raise RuntimeError("Compressed full-chip restore differs from the pre-erase backup")

        self.change_obk_baud(115200)
        return {
            "flash_size": flash_size,
            "protected_size": protected_size,
            "backup_file": str(backup_path),
            "backup_sha256": backup_sha256,
            "target_compressed_xmodem_bytes": len(compressed_backup),
            "host_compressed_bytes": len(compressed_writable),
            "final_compare": "exact",
        }

    def restore_compressed_backup(self, backup_path: Path) -> Dict[str, object]:
        backup = backup_path.read_bytes()
        if self.flash_size == 0 or len(backup) != self.flash_size:
            raise RuntimeError(f"Backup size {len(backup)} does not match detected flash size {self.flash_size}")
        protected_size = 0x2000
        protected = self.read_obk_memory(0x08000000, protected_size, protected_size, 5.0)
        if protected != backup[:protected_size]:
            raise RuntimeError("Current protected first 8 KiB differs from the backup; refusing partial restore")

        self.change_obk_baud(460800)
        writable = backup[protected_size:]
        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed_writable = compressor.compress(writable) + compressor.flush()
        print(f"Restoring backup: {len(writable)} writable bytes from {len(compressed_writable)} compressed bytes...")
        _, status = self.execute_obk_command(0x97, struct.pack("<II", protected_size, len(writable)), timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compressed backup restore command failed with status {status}")
        self.xmodem_send(compressed_writable, initial_wait=10.0, block_size=1024, response_timeout=15.0)

        read_payload = struct.pack("<II", 0, self.flash_size) + bytes((5,))
        _, status = self.execute_obk_command(0x96, read_payload, timeout=5.0)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"Compressed backup verification command failed with status {status}")
        compressed_final = self.xmodem_receive(None, timeout=60.0)
        restored = zlib.decompress(compressed_final, wbits=-15)
        if restored != backup:
            raise RuntimeError("Restored flash differs from the supplied backup")
        self.change_obk_baud(115200)
        return {
            "backup_file": str(backup_path),
            "backup_sha256": hashlib.sha256(backup).hexdigest(),
            "host_compressed_bytes": len(compressed_writable),
            "final_compare": "exact",
        }

    def rom_preflight(self) -> Dict[str, str]:
        out: Dict[str, str] = {}
        try:
            fid = self.execute_command(0x3C, expected_len=10, timeout=1.0)
            out["flash_id_raw_hex"] = fid.hex(" ")
            out["flash_id_raw_ascii"] = ascii_preview(fid, len(fid))
            print(f"ROM command 0x3C flash-id reply: {fid.hex(' ')} | {ascii_preview(fid, len(fid))}")
        except Exception as e:
            out["flash_id_error"] = str(e)
            print(f"WARN: ROM command 0x3C flash-id failed: {e}")
        try:
            romv = self.execute_command(0x3E, expected_len=3, timeout=1.0)
            out["rom_version_raw_hex"] = romv.hex(" ")
            out["rom_version_raw_ascii"] = ascii_preview(romv, len(romv))
            print(f"ROM command 0x3E version reply: {romv.hex(' ')} | {ascii_preview(romv, len(romv))}")
        except Exception as e:
            out["rom_version_error"] = str(e)
            print(f"WARN: ROM command 0x3E version failed: {e}")
        return out

    def xmodem_send(self, image: bytes, initial_wait: float = 20.0, max_retries: int = 16,
                    block_size: int = 1024, response_timeout: float = 5.0) -> None:
        if block_size == 1024:
            start_byte = STX
            pad = PAD_FF
        elif block_size == 128:
            start_byte = SOH
            pad = PAD_FF
        else:
            raise ValueError("block_size must be 1024 or 128")

        print(f"Waiting for ROM XMODEM receiver ('C' or NAK), block_size={block_size}...")
        c = self.wait_for_any((CRCCHR, NAK), initial_wait, "XMODEM start")
        use_crc = (c == CRCCHR)
        print(f"XMODEM receiver ready: {'CRC mode' if use_crc else 'checksum mode'}")

        block_no = 1
        offset = 0
        total_blocks = (len(image) + block_size - 1) // block_size
        while offset < len(image):
            block = image[offset:offset + block_size]
            if len(block) < block_size:
                block = block + bytes([pad]) * (block_size - len(block))
            pkt = bytearray([start_byte, block_no & 0xFF, 0xFF - (block_no & 0xFF)])
            pkt.extend(block)
            if use_crc:
                pkt.extend(struct.pack(">H", crc16_xmodem(block)))
            else:
                pkt.append(sum(block) & 0xFF)

            for attempt in range(1, max_retries + 1):
                self.ser.write(pkt)
                self.ser.flush()
                try:
                    r = self.wait_for_any((ACK, NAK, CAN, CRCCHR), response_timeout, "XMODEM ACK/NAK/CAN")
                except SerialTimeout:
                    r = None
                if r == ACK:
                    offset += block_size
                    if total_blocks <= 16 or block_no == 1 or (block_no % 64) == 0 or offset >= len(image):
                        print(f"XMODEM block {block_no:03d}/{total_blocks:03d} ACK")
                    block_no = (block_no + 1) & 0xFF
                    break
                if r == CAN:
                    tail = self.drain(0.2, max_total=0.5, max_bytes=128)
                    raise RuntimeError(f"Receiver cancelled XMODEM transfer; tail={tail.hex(' ')} | {ascii_preview(tail)}")
                if self.verbose:
                    print(f"XMODEM block {block_no} retry {attempt}, response={r}")
            else:
                raise RuntimeError(f"XMODEM failed at block {block_no}")

        for attempt in range(1, max_retries + 1):
            self.ser.write(bytes([EOT]))
            self.ser.flush()
            try:
                r = self.wait_for_any((ACK, NAK, CAN, CRCCHR), 5.0, "XMODEM final ACK")
            except SerialTimeout:
                r = None
            if r == ACK:
                print("XMODEM EOT ACK")
                return
            if r == CAN:
                tail = self.drain(0.2, max_total=0.5, max_bytes=128)
                raise RuntimeError(f"Receiver cancelled at EOT; tail={tail.hex(' ')} | {ascii_preview(tail)}")
            if self.verbose:
                print(f"XMODEM EOT retry {attempt}, response={r}")
        raise RuntimeError("XMODEM EOT not acknowledged")

def analyse_blob(name: str, addr: int, data: bytes) -> Dict[str, object]:
    sha = hashlib.sha256(data).hexdigest()
    c32 = crc32(data)
    all_ff = all(b == 0xFF for b in data)
    all_00 = all(b == 0x00 for b in data)
    words = []
    for off in range(0, min(len(data), 32), 4):
        if off + 4 <= len(data):
            words.append(f"0x{struct.unpack_from('<I', data, off)[0]:08x}")
    notes: List[str] = []
    if len(data) >= 16:
        magic, img_attr, img_addr, img_len = struct.unpack_from("<IIII", data, 0)
        if magic == 0xA0FFFF9F:
            notes.append(f"W800 image header at start; img_addr=0x{img_addr:08x}, img_len=0x{img_len:x}")
    if all_ff:
        notes.append("all 0xff")
    if all_00:
        notes.append("all 0x00")
    return {
        "name": name,
        "addr": f"0x{addr:08x}",
        "size": len(data),
        "crc32": f"0x{c32:08x}",
        "sha256": sha,
        "first64": hexdump_prefix(data, 64),
        "first64_ascii": ascii_preview(data, 64),
        "first_words_le": words,
        "notes": notes,
    }


def parse_extra_read(text: str) -> Tuple[str, int, int, bool]:
    # Syntax: ADDR:SIZE[:NAME]
    parts = text.split(":")
    if len(parts) not in (2, 3):
        raise argparse.ArgumentTypeError("Use ADDR:SIZE[:NAME], e.g. 0x00000000:0x100:rom_probe")
    addr = parse_int(parts[0])
    size = parse_int(parts[1])
    if size <= 0:
        raise argparse.ArgumentTypeError("SIZE must be positive")
    name = parts[2] if len(parts) == 3 and parts[2] else f"extra_{addr:08x}_{size:08x}"
    safe = all(ch.isalnum() or ch in ("_", "-", ".") for ch in name)
    if not safe:
        raise argparse.ArgumentTypeError("NAME may only contain letters, digits, underscore, dash, dot")
    return name, addr, size, False


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Probe W800 ROM/RAM/QFLASH through the custom raw-memory RAM stub")
    ap.add_argument("--port", required=True, help="Serial port, e.g. COM27 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200, help="Serial baud rate, default 115200")
    ap.add_argument("--stub", default="W800_RawMem_Stub.bin", help="Path to compressed or raw W800 custom raw-memory stub image")
    ap.add_argument("--out", default="w800_probe_out", help="Output directory")
    ap.add_argument("--chunk", type=lambda s: parse_int(s), default=0x400, help="Read chunk size, default 0x400")
    ap.add_argument("--timeout", type=float, default=5.0, help="Per-command/read timeout in seconds")
    ap.add_argument("--entry-timeout", type=float, default=60.0, help="Seconds to try W800 AT+Z/ESC entry sequence")
    ap.add_argument("--xmodem-wait", type=float, default=30.0, help="Seconds to wait for ROM XMODEM 'C' after sync")
    ap.add_argument("--manual-reset", action="store_true", help="Prompt before running the W800 sync/entry sequence")
    ap.add_argument("--no-atz-esc", action="store_true", help="Do not send the W800 AT+Z/ESC entry sequence; only wait for CCCC")
    ap.add_argument("--reset-esc-only", action="store_true", help="Skip AT+Z and send ESC while waiting for a physical reset")
    ap.add_argument("--skip-rom-preflight", action="store_true", help="Skip ROM 0x3C/0x3E commands before uploading stub")
    ap.add_argument("--no-post-stub-sync", action="store_true", help="Do not wait for post-upload CCCC from the RAM stub")
    ap.add_argument("--xmodem-128", action="store_true", help="Use 128-byte SOH XMODEM instead of the default 1K STX XMODEM")
    ap.add_argument("--no-upload", action="store_true", help="Assume the stub is already running; skip ROM sync and XMODEM upload")
    ap.add_argument("--skip-obk-tests", action="store_true", help="Skip OBK 0xA5 protocol compatibility tests")
    ap.add_argument("--test-flash-mutation", action="store_true", help="Destructively test OBK sector write and erase in an erased scratch sector")
    ap.add_argument("--test-baud-flash-mutation", action="store_true", help="Run the flash mutation test at each supported Easy Flasher baud")
    ap.add_argument("--benchmark-crc", action="store_true", help="Benchmark stub CRC32 over representative flash ranges")
    ap.add_argument("--scratch-offset", type=lambda s: parse_int(s), default=0x180000, help="QFLASH offset for destructive testing, default 0x180000")
    ap.add_argument("--test-compression-stress", action="store_true", help="Test a large compressed write/read/erase cycle in an erased flash range")
    ap.add_argument("--compression-stress-size", type=lambda s: parse_int(s), default=0x40000, help="Compressed stress-test size, default 0x40000")
    ap.add_argument("--test-full-chip-cycle", action="store_true", help="Back up, erase, restore, and verify the complete QFLASH through common commands")
    ap.add_argument("--cycle-baud", type=int, default=460800, help="Baud for the uncompressed full-chip cycle, default 460800")
    ap.add_argument("--test-compressed-full-chip-cycle", action="store_true", help="Back up, erase, restore, and verify complete QFLASH through compressed common commands")
    ap.add_argument("--restore-compressed-backup", type=Path, help="Restore and exactly verify a full-chip backup through compressed common commands")
    ap.add_argument("--probe-only", action="store_true", help="Only do 0x100-byte probes; do not dump 20KB ROM or 8KB parameter area")
    ap.add_argument("--include-alias-1ff00000", action="store_true", help="Also probe candidate ROM alias 0x1FF00000")
    ap.add_argument("--read", action="append", type=parse_extra_read, default=[], help="Add extra read ADDR:SIZE[:NAME]")
    ap.add_argument("--verbose", action="store_true", help="Print serial byte-level detail")
    args = ap.parse_args(argv)

    stub_path = Path(args.stub)
    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    reads: List[Tuple[str, int, int, bool]] = []
    for name, addr, size, must_match_stub in DEFAULT_READS:
        if args.probe_only:
            size = min(size, 0x100)
            name = name.replace("00005000", "00000100").replace("00002000", "00000100")
        reads.append((name, addr, size, must_match_stub))
    if args.include_alias_1ff00000:
        reads.append(ALIAS_READ)
    reads.extend(args.read)

    print(f"Opening {args.port} @ {args.baud}")
    probe = W800Probe(args.port, args.baud, args.timeout, verbose=args.verbose)
    manifest: Dict[str, object] = {
        "tool_version": "w800_custom_raw_stub_probe_v0.7",
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "port": args.port,
        "baud": args.baud,
        "stub": str(stub_path),
        "reads": [],
    }

    try:
        if not args.no_upload:
            image = load_stub_image(stub_path)
            magic, img_attr, img_addr, img_len = struct.unpack_from("<IIII", image, 0)
            print(f"Stub image: {len(image)} bytes; magic=0x{magic:08x}; img_addr=0x{img_addr:08x}; img_len=0x{img_len:x}")
            manifest["stub_image"] = {
                "image_len": len(image),
                "magic": f"0x{magic:08x}",
                "img_addr": f"0x{img_addr:08x}",
                "img_len": img_len,
                "crc32": f"0x{crc32(image):08x}",
                "sha256": hashlib.sha256(image).hexdigest(),
            }
            if args.manual_reset:
                input("Put/reset the W800 as usual, then press Enter here. The script will now do W800 CCCC/AT+Z/ESC sync...")

            probe.sync_w800_download_mode(entry_timeout=args.entry_timeout, use_atz_esc=not args.no_atz_esc,
                                          reset_esc_only=args.reset_esc_only)

            if not args.skip_rom_preflight:
                manifest["rom_preflight"] = probe.rom_preflight()


            block_size = 128 if args.xmodem_128 else 1024
            probe.xmodem_send(image, initial_wait=args.xmodem_wait, block_size=block_size)

            if not args.no_post_stub_sync:
                probe.sync_after_stub_upload(timeout=20.0)
            else:
                leftover = probe.drain(0.5)
                if leftover:
                    print(f"Post-upload RX leftover: {leftover.hex(' ')} | {ascii_preview(leftover)}")

        else:
            print("Skipping upload; assuming RAM stub is already running.")
            probe.drain(0.2)

        if not args.skip_obk_tests:
            print("Testing OBK custom-stub command surface...")
            manifest["obk_protocol"] = probe.test_obk_protocol()
            print("OBK custom-stub protocol tests passed.")
            if args.benchmark_crc:
                manifest["obk_crc32_benchmark"] = probe.benchmark_crc32()
            if args.test_flash_mutation:
                print(f"Testing flash write/erase in scratch sector 0x{args.scratch_offset:08x}...")
                manifest["obk_flash_mutation"] = probe.test_obk_flash_mutation(args.scratch_offset)
                print("OBK flash write/erase tests passed and scratch sector was restored.")
            if args.test_baud_flash_mutation:
                baud_results: Dict[str, object] = {}
                for baud in (115200, 230400, 460800, 921600, 1500000, 2000000):
                    if baud != probe.ser.baudrate:
                        probe.change_obk_baud(baud)
                    print(f"Testing flash write/read at {baud} baud...")
                    baud_results[str(baud)] = probe.test_obk_flash_mutation(args.scratch_offset)
                if probe.ser.baudrate != 115200:
                    probe.change_obk_baud(115200)
                manifest["obk_baud_flash_mutation"] = baud_results
                print("OBK flash mutation passed at every supported Easy Flasher baud.")
            if args.test_compression_stress:
                print(f"Testing compressed stress range 0x{args.scratch_offset:08x}+0x{args.compression_stress_size:x}...")
                manifest["obk_compression_stress"] = probe.test_obk_compression_stress(
                    args.scratch_offset, args.compression_stress_size
                )
                print("OBK compression stress test passed and the range was restored.")
            if args.test_full_chip_cycle:
                manifest["obk_full_chip_cycle"] = probe.test_full_chip_cycle(
                    outdir / "pre_obk_erase_full_backup.bin", args.cycle_baud
                )
                print("OBK full-chip erase/restore cycle passed.")
            if args.test_compressed_full_chip_cycle:
                manifest["obk_compressed_full_chip_cycle"] = probe.test_compressed_full_chip_cycle(
                    outdir / "pre_obk_compressed_erase_full_backup.bin"
                )
                print("OBK compressed full-chip erase/restore cycle passed.")
            if args.restore_compressed_backup:
                manifest["obk_compressed_backup_restore"] = probe.restore_compressed_backup(args.restore_compressed_backup)
                print("OBK compressed backup restore passed.")
        elif args.benchmark_crc or args.test_flash_mutation or args.test_baud_flash_mutation or args.test_compression_stress or args.test_full_chip_cycle or args.test_compressed_full_chip_cycle or args.restore_compressed_backup:
            raise ValueError("CRC benchmark and flash mutation tests require OBK tests")

        read_failures: List[str] = []
        for name, addr, size, must_match_stub in reads:
            path = outdir / f"{name}.bin"
            entry: Dict[str, object] = {"name": name, "addr": f"0x{addr:08x}", "size_requested": size}
            try:
                read_timeout = max(args.timeout, 2.0 + (args.chunk / 1024.0))
                data = probe.read_obk_memory(addr, size, args.chunk, timeout=read_timeout)
                path.write_bytes(data)
                analysis = analyse_blob(name, addr, data)
                analysis["file"] = str(path)
                analysis["chunk_crc_checks"] = list(probe.current_read_metas)
                entry.update(analysis)
                if must_match_stub:
                    try:
                        stub_body = load_stub_image(stub_path)[64:64 + len(data)]
                        ok = (data == stub_body[:len(data)])
                        entry["stub_body_prefix_match"] = ok
                        print("Custom stub RAM body proof: " + ("PASS" if ok else "FAIL"))
                    except Exception as cmp_e:
                        entry["stub_body_prefix_match_error"] = str(cmp_e)
                print(f"Saved {path} crc32={entry.get('crc32')} sha256={str(entry.get('sha256'))[:16]}...")
                print(f"First 64: {entry.get('first64')}")
                print(f"ASCII    : {entry.get('first64_ascii')}")
            except Exception as e:
                err = str(e)
                entry["error"] = err
                read_failures.append(f"{name}: {err}")
                print(f"ERROR reading {name} at 0x{addr:08x}+0x{size:x}: {e}")
            cast_reads = manifest["reads"]
            assert isinstance(cast_reads, list)
            cast_reads.append(entry)

        if read_failures:
            raise RuntimeError("One or more requested reads failed: " + "; ".join(read_failures))

    finally:
        manifest["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        manifest_path = outdir / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"Manifest written: {manifest_path}")
        probe.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
