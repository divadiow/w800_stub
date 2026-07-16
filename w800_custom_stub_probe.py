#!/usr/bin/env python3
"""
W800 custom raw-memory RAM-stub probe.

v0.6: validates both WinnerMicro-compatible reads and the OBK custom-stub protocol.

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


def crc32_wm_wire(data: bytes) -> int:
    """CRC32 form observed on W800 0x4A replies: bitwise inverse of zlib CRC32.

    The C# tool compares the reply trailer against CRC.crc32_ver2(0xFFFFFFFF,...).
    The first v0.2 field result showed rx=~zlib_crc32(data), so v0.3 accepts
    this as the primary wire CRC while still accepting the direct zlib value as
    a fallback for safety.
    """
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


def w800_read_frame(addr: int, size: int) -> bytes:
    return w800_frame(0x4A, struct.pack("<II", addr & 0xFFFFFFFF, size & 0xFFFFFFFF))


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
        self.last_read_meta: Dict[str, object] = {}
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

    def sync_w800_download_mode(self, entry_timeout: float = 60.0, use_atz_esc: bool = True) -> None:
        # This intentionally follows BK7231Flasher's W800 path rather than the generic CCCC wait.
        self.set_lines(rts=False, dtr=False)
        self.reset_buffers()

        if self.wait_for_cccc(0.5, preserve_count_on_timeout=False, label="initial W800 prompt check"):
            self.reset_buffers()
            return

        if not use_atz_esc:
            raise SerialTimeout("No CCCC prompt and --no-atz-esc was set")

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
        raise SerialTimeout(
            "W800 sync failed: no CCCC download prompt after AT+Z/ESC sequence. "
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

    def execute_command_variable(self, cmd: int, payload: bytes = b"", settle: float = 0.25, drain: float = 0.15, max_total: float = 1.5, max_bytes: int = 1024) -> bytes:
        """Send a command whose reply length is unknown.

        Generic bounded variable-response drain. The hard max_total cap is
        separate from settle/drain so continuous prompt bytes cannot extend forever.
        """
        frame = w800_frame(cmd, payload)
        self.reset_buffers()
        self.ser.write(frame)
        self.ser.flush()
        time.sleep(settle)
        return self.drain(drain, max_total=max_total, max_bytes=max_bytes, extend_on_data=True)

    def change_w800_baud(self, baud: int, timeout: float = 2.0) -> None:
        self.reset_buffers()
        self.ser.write(w800_frame(0x31, struct.pack("<I", baud)))
        self.ser.flush()
        self.ser.baudrate = baud
        reply = self.read_exact(1, timeout, f"W800 baud-change acknowledgement at {baud}")
        if reply != b"C":
            raise RuntimeError(f"W800 baud change to {baud} returned {reply.hex(' ')}, expected 43")

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

    def xmodem_receive(self, expected_len: int, timeout: float = 10.0) -> bytes:
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
                    if len(output) < expected_len:
                        raise RuntimeError(f"XMODEM ended after {len(output)} bytes, expected {expected_len}")
                    return bytes(output[:expected_len])
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
        results["flash_id_hex"] = flash_id.hex(" ")

        flash_control = self.read_memory_once(0x08000000, 0x100, 3.0)
        crc_reply, status = self.execute_obk_command(0x8F, struct.pack("<II", 0, len(flash_control)), timeout=3.0)
        if status != OBK_STATUS_SUCCESS or len(crc_reply) != 4:
            raise RuntimeError(f"OBK flash CRC32 failed with status {status} and {len(crc_reply)} data bytes")
        target_crc = struct.unpack("<I", crc_reply)[0]
        expected_crc = crc32_wm_wire(flash_control)
        if target_crc != expected_crc:
            raise RuntimeError(f"OBK flash CRC32 mismatch: target=0x{target_crc:08x}, expected=0x{expected_crc:08x}")
        results["flash_crc32"] = f"0x{target_crc:08x}"

        _, status = self.execute_obk_command(0x92, struct.pack("<II", 0, len(flash_control)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK flash XMODEM read command failed with status {status}")
        flash_xmodem = self.xmodem_receive(len(flash_control))
        if flash_xmodem != flash_control:
            raise RuntimeError("OBK flash XMODEM data differs from WinnerMicro 0x4A control read")
        results["flash_xmodem_read"] = "ok"

        rom_control = self.read_memory_once(0x00000000, 0x100, 3.0)
        _, status = self.execute_obk_command(0x98, struct.pack("<II", 0, len(rom_control)))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK raw XMODEM read command failed with status {status}")
        rom_xmodem = self.xmodem_receive(len(rom_control))
        if rom_xmodem != rom_control:
            raise RuntimeError("OBK raw XMODEM data differs from WinnerMicro 0x4A control read")
        results["raw_xmodem_read"] = "ok"

        _, status = self.execute_obk_command(0x07, struct.pack("<I", 460800))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK baud change to 460800 failed with status {status}")
        self.ser.baudrate = 460800
        _, status = self.execute_obk_command(0x00)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError("OBK sync failed at 460800 baud")
        _, status = self.execute_obk_command(0x07, struct.pack("<I", 115200))
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError(f"OBK baud restore to 115200 failed with status {status}")
        self.ser.baudrate = 115200
        _, status = self.execute_obk_command(0x00)
        if status != OBK_STATUS_SUCCESS:
            raise RuntimeError("OBK sync failed after restoring 115200 baud")
        results["baud_460800"] = "ok"

        data, status = self.execute_obk_command(0x99)
        if data or status != OBK_STATUS_TYPE_ERROR:
            raise RuntimeError(f"OBK unsupported eFuse command returned status {status} and {len(data)} data bytes")
        results["efuse_command"] = "unsupported"
        return results

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

    def xmodem_send(self, image: bytes, initial_wait: float = 20.0, max_retries: int = 16, block_size: int = 1024) -> None:
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
                    r = self.wait_for_any((ACK, NAK, CAN, CRCCHR), 5.0, "XMODEM ACK/NAK/CAN")
                except SerialTimeout:
                    r = None
                if r == ACK:
                    offset += block_size
                    print(f"XMODEM block {block_no:03d}/{total_blocks:03d} ACK")
                    block_no = (block_no + 1) & 0xFF
                    break
                if r == CAN:
                    raise RuntimeError("Receiver cancelled XMODEM transfer")
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
                raise RuntimeError("Receiver cancelled at EOT")
            if self.verbose:
                print(f"XMODEM EOT retry {attempt}, response={r}")
        raise RuntimeError("XMODEM EOT not acknowledged")

    def read_memory_once(self, addr: int, size: int, timeout: float) -> bytes:
        frame = w800_read_frame(addr, size)
        self.reset_buffers()
        self.ser.write(frame)
        self.ser.flush()
        reply = self.read_exact(size + 4, timeout, f"read reply 0x{addr:08x}+0x{size:x}")
        data = reply[:-4]
        rx_crc = struct.unpack("<I", reply[-4:])[0]
        calc_std = crc32(data)
        calc_wm = crc32_wm_wire(data)
        if rx_crc == calc_wm:
            mode = "wm_wire_inverted_zlib"
        elif rx_crc == calc_std:
            mode = "zlib_direct"
        else:
            raise RuntimeError(
                f"CRC32 mismatch for 0x{addr:08x}+0x{size:x}: "
                f"rx=0x{rx_crc:08x}, wm_wire=0x{calc_wm:08x}, zlib=0x{calc_std:08x}, "
                f"tail={reply[-16:].hex(' ')}"
            )
        self.last_read_meta = {
            "addr": f"0x{addr:08x}",
            "size": size,
            "wire_crc32_rx": f"0x{rx_crc:08x}",
            "wire_crc32_mode": mode,
            "zlib_crc32": f"0x{calc_std:08x}",
        }
        return data

    def read_memory(self, addr: int, size: int, chunk_size: int, timeout: float) -> bytes:
        out = bytearray()
        remaining = size
        cur = addr
        self.current_read_metas = []
        while remaining:
            n = min(chunk_size, remaining)
            print(f"Reading 0x{cur:08x} +0x{n:x}...")
            data = self.read_memory_once(cur, n, timeout)
            self.current_read_metas.append(dict(self.last_read_meta))
            out.extend(data)
            cur += n
            remaining -= n
        return bytes(out)


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
    ap.add_argument("--skip-rom-preflight", action="store_true", help="Skip ROM 0x3C/0x3E commands before uploading stub")
    ap.add_argument("--no-post-stub-sync", action="store_true", help="Do not wait for post-upload CCCC from the RAM stub")
    ap.add_argument("--xmodem-128", action="store_true", help="Use 128-byte SOH XMODEM instead of the default 1K STX XMODEM")
    ap.add_argument("--no-upload", action="store_true", help="Assume the stub is already running; skip ROM sync and XMODEM upload")
    ap.add_argument("--skip-obk-tests", action="store_true", help="Skip OBK 0xA5 protocol compatibility tests")
    ap.add_argument("--test-reset", action="store_true", help="Issue stub command 0x3F after all reads and verify the reset acknowledgement")
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
        "tool_version": "w800_custom_raw_stub_probe_v0.6",
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

            probe.sync_w800_download_mode(entry_timeout=args.entry_timeout, use_atz_esc=not args.no_atz_esc)

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

            # Positive proof that the custom stub, not the ROM prompt, is consuming commands.
            try:
                stubv = probe.execute_command(0x3E, expected_len=10, timeout=1.5)
                manifest["custom_stub_version_raw_hex"] = stubv.hex(" ")
                manifest["custom_stub_version_raw_ascii"] = ascii_preview(stubv, len(stubv))
                print(f"Custom stub command 0x3E reply: {stubv.hex(' ')} | {ascii_preview(stubv, len(stubv))}")
            except Exception as e:
                manifest["custom_stub_version_error"] = str(e)
                print(f"WARN: custom stub command 0x3E did not reply as expected: {e}")

            try:
                stub_fid = probe.execute_command(0x3C, expected_len=10, timeout=1.5)
                manifest["custom_stub_flash_id_raw_hex"] = stub_fid.hex(" ")
                manifest["custom_stub_flash_id_raw_ascii"] = ascii_preview(stub_fid, len(stub_fid))
                print(f"Custom stub command 0x3C flash-id reply: {stub_fid.hex(' ')} | {ascii_preview(stub_fid, len(stub_fid))}")
            except Exception as e:
                manifest["custom_stub_flash_id_error"] = str(e)
                print(f"WARN: custom stub command 0x3C did not reply as expected: {e}")

            probe.change_w800_baud(460800)
            high_baud_version = probe.execute_command(0x3E, expected_len=10, timeout=1.5)
            if high_baud_version != b"R:W800RAW6":
                raise RuntimeError(f"Unexpected W800 version reply at 460800 baud: {high_baud_version!r}")
            probe.change_w800_baud(115200)
            manifest["winner_micro_baud_460800"] = "ok"

            if not args.skip_obk_tests:
                print("Testing OBK custom-stub command surface...")
                manifest["obk_protocol"] = probe.test_obk_protocol()
                print("OBK custom-stub protocol tests passed.")
        else:
            print("Skipping upload; assuming RAM stub is already running.")
            probe.drain(0.2)

        for name, addr, size, must_match_stub in reads:
            path = outdir / f"{name}.bin"
            entry: Dict[str, object] = {"name": name, "addr": f"0x{addr:08x}", "size_requested": size}
            try:
                read_timeout = max(args.timeout, 2.0 + (args.chunk / 1024.0))
                data = probe.read_memory(addr, size, args.chunk, timeout=read_timeout)
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
                if "53 53 53 43" in err or "SSSCCCC" in err:
                    entry["status_interpretation"] = "Target rejected this 0x4A read; tail begins with ASCII S status followed by C prompt bytes. On W800 ROM docs, S means command parameter error."
                print(f"ERROR reading {name} at 0x{addr:08x}+0x{size:x}: {e}")
            cast_reads = manifest["reads"]
            assert isinstance(cast_reads, list)
            cast_reads.append(entry)

        if args.test_reset:
            reset_reply = probe.execute_command(0x3F, expected_len=1, timeout=2.0)
            if reset_reply != b"C":
                raise RuntimeError(f"W800 reset returned {reset_reply.hex(' ')}, expected 43")
            time.sleep(1.0)
            post_reset = probe.drain(0.2, max_total=1.0, max_bytes=512)
            manifest["reset_test"] = {
                "ack": reset_reply.hex(" "),
                "post_reset_bytes": len(post_reset),
                "post_reset_ascii": ascii_preview(post_reset, len(post_reset)),
            }
            print(f"W800 reset command acknowledged; observed {len(post_reset)} post-reset bytes.")

    finally:
        manifest["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        manifest_path = outdir / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"Manifest written: {manifest_path}")
        probe.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
