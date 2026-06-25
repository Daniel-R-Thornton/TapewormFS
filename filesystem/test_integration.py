"""
Integration test: tapefs ↔ DummyMCU over serial pipe.

DummyMCU runs as subprocess, talking framed packets over a binary pipe
(stdin/stdout with zero buffering — same protocol as a real UART).

Usage:  python3 test_integration.py
"""

import os, sys, time, struct, select, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from tapefs import (
    Filesystem, Block, BlockType, Error,
    serialise, deserialise, RawBlock,
)
from dummy_mcu import encode_packet, decode_packet


def start_mcu():
    """Start DummyMCU as subprocess, return (proc, stdin_fd, stdout_fd)."""
    proc = subprocess.Popen(
        [sys.executable, '-c', fr'''
import sys, os
sys.path.insert(0, "{HERE}")
from dummy_mcu import DummyMCU
mcu = DummyMCU()
rf = os.fdopen(0, "rb", buffering=0)
wf = os.fdopen(1, "wb", buffering=0)
mcu._run_stream(rf, wf)
'''],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(0.5)
    return proc


def send(proc, cmd_id, payload=b''):
    proc.stdin.write(encode_packet(cmd_id, payload))
    proc.stdin.flush()


def recv(proc, expected_id, timeout=5.0):
    """Read from proc.stdout until we get a matching response packet."""
    buf = b''
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r, _, _ = select.select([proc.stdout], [], [], 0.1)
        if r:
            chunk = proc.stdout.read1(4096)
            if not chunk:
                return None  # EOF
            buf += chunk

        # Find start marker
        idx = buf.find(b'\xFE')
        if idx < 0:
            continue
        buf = buf[idx:]

        pkt = decode_packet(buf)
        if pkt is None:
            buf = buf[1:]
            continue
        if pkt['cmd_id'] == expected_id:
            return pkt['payload']
        # Wrong packet — discard buffer and wait for next
        buf = b''
    return None  # timeout


class MultiTest:
    """Runs multiple tests, sharing one MCU process."""

    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.proc = None

    def test(self, name, fn):
        try:
            print(f"  {name:<55} ... ", end='', flush=True)
            fn()
            print("OK")
            self.passed += 1
        except Exception as e:
            print(f"FAIL: {e}")
            self.failed += 1

    def run(self):
        self.proc = start_mcu()

        # ---- 1. PING --------------------------------------------- #
        def ping():
            send(self.proc, 0x01)
            rsp = recv(self.proc, 0x81, timeout=3.0)
            assert rsp and b'TapewormFS' in rsp, f"bad PONG"

        self.test("PING (alive check)", ping)

        # ---- 2. Block write/read --------------------------------- #
        def block_rw():
            block = Block(type=BlockType.DATA, seq_no=42, data=b"Serial test!")
            raw = serialise(block)

            send(self.proc, 0x05, raw.bytes)
            rsp = recv(self.proc, 0x85)
            assert rsp and rsp[0] == 0x00, "WRITE_BLOCK NAK"

            send(self.proc, 0x07)  # FLUSH
            rsp = recv(self.proc, 0x87)
            assert rsp and rsp[0] == 0x00, "FLUSH NAK"

            send(self.proc, 0x06)  # READ_NEXT
            rsp = recv(self.proc, 0x86, timeout=5.0)
            assert rsp and len(rsp) > 5, f"READ_NEXT NAK"

            # Reconstruct: rsp = block_type(1) + block_data(N) + crc32(4)
            reconstructed = rsp[0:1] + rsp[1:-4]
            restored, err = deserialise(RawBlock(bytes=reconstructed))
            assert err == Error.OK, f"deserialise error: {err}"
            assert restored.data == b"Serial test!", f"data mismatch"

        self.test("Block write/read", block_rw)

        # ---- 3. Full filesystem (via raw callbacks) ------------ #
        def fs_ops():
            from tapefs import Directory as Dir
            from dummy_mcu import DummyMCU
            # Use a fresh DummyMCU with raw callbacks (not packet protocol)
            mcu = DummyMCU()
            fs = Filesystem(mcu.raw_write, mcu.raw_read, mcu.raw_seek)
            assert fs.format() == Error.OK, "format"

            content = b"Hello from raw filesystem!"
            err = fs.write_file("serial.txt", content)
            assert err == Error.OK, f"write: {err}"

            data, err = fs.read_file("serial.txt")
            assert err == Error.OK, f"read: {err}"
            assert data == content, f"data mismatch"

            dir_, err = fs.list_files()
            assert err == Error.OK
            assert dir_.count == 1
            e = dir_.find("serial.txt")
            assert e and e.file_size == len(content)

        self.test("Full filesystem (raw callbacks)", fs_ops)

        # ---- 4. Multiple files (via raw callbacks) -------------- #
        def multi():
            from dummy_mcu import DummyMCU
            mcu = DummyMCU()
            fs = Filesystem(mcu.raw_write, mcu.raw_read, mcu.raw_seek)
            fs.format()

            files = {"a.txt": b"Hello", "b.bin": bytes(range(64))}
            for name, data in files.items():
                assert fs.write_file(name, data) == Error.OK
            for name, expected in files.items():
                data, err = fs.read_file(name)
                assert err == Error.OK and data == expected

            dir_, err = fs.list_files()
            assert err == Error.OK and dir_.count == len(files)

        self.test("Multiple files", multi)

        # ---- 5. CRC error handling -------------------------------- #
        def crc_err():
            good = encode_packet(0x01)
            bad = bytearray(good)
            bad[-1] ^= 0xFF  # corrupt CRC
            self.proc.stdin.write(bytes(bad))
            self.proc.stdin.flush()
            time.sleep(0.3)
            r, _, _ = select.select([self.proc.stdout], [], [], 1.0)
            print(" (bad CRC: " + ("responded" if r else "dropped") + ")", end='')

            send(self.proc, 0x01)  # valid PING
            rsp = recv(self.proc, 0x81, timeout=3.0)
            assert rsp and b'TapewormFS' in rsp

        self.test("CRC error handling", crc_err)

        # ---- Cleanup ---------------------------------------------- #
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except:
            self.proc.kill()


if __name__ == "__main__":
    print("=" * 70)
    print("TapewormFS Integration Test — Serial (pipe) to DummyMCU")
    print("=" * 70)
    print()

    t = MultiTest()
    t.run()

    print()
    print("=" * 70)
    print(f"Results: {t.passed} passed, {t.failed} failed")
    print("=" * 70)
    sys.exit(0 if t.failed == 0 else 1)
