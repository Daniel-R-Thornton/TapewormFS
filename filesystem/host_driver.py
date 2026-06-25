"""
host_driver.py — Mount a folder as the contents of a tape cassette.

The host driver presents the tape contents as a normal folder on your
computer.  Files you copy to the mount point are synced to cassette
in the background.

Usage:
    # Mount tape contents to ./tape_mount/ (FUSE required)
    python3 host_driver.py mount ./tape_mount --port /dev/ttyUSB0

    # Sync a local folder to/from tape (no FUSE needed)
    python3 host_driver.py sync ./tape_backup --port /dev/ttyUSB0

    # List files on tape
    python3 host_driver.py ls --port /dev/ttyUSB0

    # Format tape
    python3 host_driver.py format --port /dev/ttyUSB0

Architecture:
  ┌────────────────────┐    UART (framed packets)    ┌──────────────┐
  │  /mnt/tape0/       │◀──────────────────────────▶│  ESP32       │
  │  (FUSE mount)      │     [0xFE|len|cmd|         │  (FSK modem) │
  │                    │      payload|crc16]        │              │
  │  /tmp/tapewormfs/  │                            │  MCP4725 DAC │
  │  (disk cache)      │                            │  Onboard ADC │
  └────────────────────┘                            └──────────────┘
"""

import os
import sys
import time
import json
import shutil
import struct
import threading
import tempfile
import argparse
from pathlib import Path
from datetime import datetime

# Add parent dir for tapefs imports
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from tapefs import (
    Filesystem, Directory, BlockType, Error,
    serialise, deserialise, RawBlock
)

# ======================================================================
#  Cache management
# ======================================================================

CACHE_DIR = "/tmp/tapewormfs"

class TapeCache:
    """
    Disk-backed cache for tape contents.

    Directory:
      /tmp/tapewormfs/<session>/
        manifest.json   ← file list + metadata
        files/          ← cached file data
        dirty/          ← files not yet synced to tape
    """

    def __init__(self):
        os.makedirs(CACHE_DIR, exist_ok=True)
        self.session = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.cache_path = os.path.join(CACHE_DIR, self.session)
        self.files_path = os.path.join(self.cache_path, "files")
        self.dirty_path = os.path.join(self.cache_path, "dirty")
        self.manifest_path = os.path.join(self.cache_path, "manifest.json")
        os.makedirs(self.files_path, exist_ok=True)
        os.makedirs(self.dirty_path, exist_ok=True)

        # Current manifest
        self.manifest = {}  # filename -> {"size": N, "cached": bool, "dirty": bool}

        # Link current
        current_link = os.path.join(CACHE_DIR, "current")
        try:
            os.unlink(current_link)
        except FileNotFoundError:
            pass
        os.symlink(self.cache_path, current_link)

    def save_manifest(self):
        with open(self.manifest_path, 'w') as f:
            json.dump(self.manifest, f, indent=2)

    def has_file(self, filename: str) -> bool:
        return filename in self.manifest and self.manifest[filename].get("cached", False)

    def get_file(self, filename: str) -> bytes | None:
        path = os.path.join(self.files_path, filename)
        if os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
        return None

    def put_file(self, filename: str, data: bytes):
        path = os.path.join(self.files_path, filename)
        with open(path, 'wb') as f:
            f.write(data)
        if filename not in self.manifest:
            self.manifest[filename] = {}
        self.manifest[filename]["size"] = len(data)
        self.manifest[filename]["cached"] = True
        self.save_manifest()

    def mark_dirty(self, filename: str):
        """Mark a file as needing sync to tape."""
        touch_path = os.path.join(self.dirty_path, filename)
        with open(touch_path, 'w') as f:
            f.write("dirty")
        if filename not in self.manifest:
            self.manifest[filename] = {}
        self.manifest[filename]["dirty"] = True
        self.save_manifest()

    def mark_clean(self, filename: str):
        """Mark a file as synced to tape."""
        dirty_file = os.path.join(self.dirty_path, filename)
        try:
            os.remove(dirty_file)
        except FileNotFoundError:
            pass
        if filename in self.manifest:
            self.manifest[filename]["dirty"] = False
            self.save_manifest()

    @property
    def dirty_files(self) -> list[str]:
        return os.listdir(self.dirty_path)


# ======================================================================
#  Serial transport (UART / stdio)
# ======================================================================

def open_serial(port: str, baud: int = 115200):
    """Open a serial port. Falls back to stdio for testing."""
    if port == "stdio":
        return sys.stdin.buffer, sys.stdout.buffer
    try:
        import serial
        ser = serial.Serial(port=port, baudrate=baud, timeout=1.0)
        return ser, ser
    except ImportError:
        print("pyserial not installed. Try: pip install pyserial", file=sys.stderr)
        print("Or use --port stdio for subprocess testing", file=sys.stderr)
        sys.exit(1)

def close_serial(r, w):
    if r != sys.stdin.buffer:
        r.close()


# ======================================================================
#  Packet protocol (same format as DummyMCU and C++ TcpTransport)
# ======================================================================

START = 0xFE

def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x8005
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc

def escape(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b == 0xFE: out.extend([0xFD, 0x01])
        elif b == 0xFD: out.extend([0xFD, 0x02])
        else: out.append(b)
    return bytes(out)

def unescape(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFD and i + 1 < len(data):
            if data[i+1] == 0x01: out.append(0xFE)
            elif data[i+1] == 0x02: out.append(0xFD)
            i += 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)

def encode_packet(cmd_id: int, payload: bytes = b'') -> bytes:
    inner = struct.pack('<H', 1 + len(payload) + 2)
    inner += bytes([cmd_id]) + payload
    inner += struct.pack('<H', crc16(inner))
    return bytes([START]) + escape(inner)

def decode_packet(data: bytes) -> dict | None:
    if not data or data[0] != START: return None
    inner = unescape(data[1:])
    if len(inner) < 5: return None
    length = struct.unpack_from('<H', inner, 0)[0]
    if len(inner) < 2 + length: return None
    cmd_id = inner[2]
    payload = inner[3:3 + length - 3]
    stored = struct.unpack_from('<H', inner, 3 + length - 2)[0]
    if crc16(inner[:2 + length - 2]) != stored: return None
    return {'cmd_id': cmd_id, 'payload': payload}


# ======================================================================
#  ESP32 connection
# ======================================================================

class Esp32Connection:
    """Wraps a serial port into Filesystem callbacks."""

    def __init__(self, port: str, baud: int = 115200):
        self.reader, self.writer = open_serial(port, baud)

    def close(self):
        close_serial(self.reader, self.writer)

    def send(self, cmd_id: int, payload: bytes = b''):
        self.writer.write(encode_packet(cmd_id, payload))
        self.writer.flush()

    def recv(self, expected_id: int, timeout: float = 5.0) -> bytes | None:
        import select
        buf = b''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if hasattr(self.reader, 'read'):
                r, _, _ = select.select([self.reader], [], [], 0.1)
                if r:
                    chunk = self.reader.read1(4096)
                    if not chunk: return None
                    buf += chunk
            idx = buf.find(bytes([START]))
            if idx < 0: continue
            buf = buf[idx:]
            pkt = decode_packet(buf)
            if pkt is None: buf = buf[1:]; continue
            if pkt['cmd_id'] == expected_id: return pkt['payload']
            buf = b''
        return None

    def write_block(self, block: RawBlock) -> bool:
        self.send(0x05, block.bytes)
        r = self.recv(0x85)
        return r is not None and len(r) == 1 and r[0] == 0x00

    def flush(self) -> bool:
        self.send(0x07)
        r = self.recv(0x87)
        return r is not None and len(r) == 1 and r[0] == 0x00

    def read_block(self) -> RawBlock | None:
        self.send(0x06)
        r = self.recv(0x86, timeout=120.0)
        if r is None or len(r) <= 5: return None
        return RawBlock(bytes=r[0:1] + r[1:-4])

    def seek(self, block_no: int) -> bool:
        p = struct.pack('<III', block_no, 0, block_no * 1000) + bytes([0, 255])
        self.send(0x03, p)
        r = self.recv(0x83)
        return r is not None and len(r) == 1 and r[0] == 0x00

    def make_filesystem(self) -> Filesystem:
        """Create a tapefs.Filesystem wired to this ESP32 connection."""

        def wb(block):
            ok = self.write_block(block)
            if ok:
                self.flush()  # sync after every write
            return ok

        return Filesystem(
            write_fn=wb,
            read_fn=lambda: self.read_block(),
            seek_fn=lambda n: self.seek(n),
        )


# ======================================================================
#  Host Driver
# ======================================================================

class HostDriver:
    """
    Manages the tape ↔ cache sync.
    
    Runs a background thread that flushes dirty files to tape.
    """

    def __init__(self, esp: Esp32Connection, cache: TapeCache):
        self.esp = esp
        self.cache = cache
        self.fs = esp.make_filesystem()
        self._running = False
        self._thread: threading.Thread | None = None

    def start_sync(self):
        """Start background sync thread."""
        self._running = True
        self._thread = threading.Thread(target=self._sync_loop, daemon=True)
        self._thread.start()

    def stop_sync(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=5)
        # Final flush of dirty files
        self._sync_all()

    def _sync_loop(self):
        """Background loop: sync dirty files to tape."""
        while self._running:
            time.sleep(10)  # idle 10s between checks
            if not self._running: break
            self._sync_all()

    def _sync_all(self):
        """Write all dirty files to tape."""
        for filename in self.cache.dirty_files:
            if not self._running:
                break
            data = self.cache.get_file(filename)
            if data is None:
                self.cache.mark_clean(filename)
                continue

            print(f"  [sync] Writing '{filename}' to tape ({len(data)} B)")
            err = self.fs.write_file(filename, data)
            if err == Error.OK:
                self.cache.mark_clean(filename)
                print(f"  [sync] Done")
            else:
                print(f"  [sync] Failed: {err}")

    # ---- Mount interface ------------------------------------------- #

    def list_files(self) -> list[dict]:
        """Get file listing from tape."""
        dir_, err = self.fs.list_files()
        if err != Error.OK:
            print(f"  Error reading tape directory: {err}")
            return []
        return [
            {"name": e.filename, "size": e.file_size,
             "blocks": f"{e.start_block}..{e.end_block}"}
            for e in dir_.entries
        ]

    def read_file(self, filename: str) -> bytes | None:
        """Read a file from tape (via cache)."""
        if self.cache.has_file(filename):
            return self.cache.get_file(filename)

        data, err = self.fs.read_file(filename)
        if err != Error.OK:
            return None

        self.cache.put_file(filename, data)
        return data

    def write_file(self, filename: str, data: bytes):
        """Write a file to cache, mark dirty for tape sync."""
        self.cache.put_file(filename, data)
        self.cache.mark_dirty(filename)

    def remove_file(self, filename: str):
        """Remove a file from tape."""
        # Read directory, remove entry, write back
        dir_, err = self.fs.list_files()
        if err != Error.OK:
            return False
        dir_.remove(filename)
        err = self.fs.write_directory(dir_)
        return err == Error.OK

    def format_tape(self) -> bool:
        """Format the tape."""
        return self.fs.format() == Error.OK


# ======================================================================
#  FUSE mount (requires fusepy: pip install fusepy)
# ======================================================================

def fuse_mount(mountpoint: str, esp: Esp32Connection):
    """
    Mount tape as a FUSE filesystem.
    
    Requires: pip install fusepy
    """
    try:
        import fuse
    except ImportError:
        print("fusepy not installed. Try: pip install fusepy")
        print("Or use: python3 host_driver.py sync ...  (no FUSE needed)")
        sys.exit(1)

    cache = TapeCache()
    driver = HostDriver(esp, cache)

    class TapeFS(fuse.Operations):
        """FUSE filesystem backed by cassette tape."""

        def getattr(self, path, fh=None):
            st = fuse.Stat()
            st.st_mode = 0o755
            st.st_nlink = 2

            if path == '/':
                st.st_mode = 0o755 | fuse.S_IFDIR
                return st

            name = path.lstrip('/')
            for entry in driver.list_files():
                if entry['name'] == name:
                    st.st_mode = 0o644 | fuse.S_IFREG
                    st.st_nlink = 1
                    st.st_size = entry['size']
                    return st

            return -fuse.ENOENT

        def readdir(self, path, fh):
            yield '.'
            yield '..'
            for entry in driver.list_files():
                yield entry['name']

        def open(self, path, flags):
            return 0

        def read(self, path, size, offset, fh):
            name = path.lstrip('/')
            data = driver.read_file(name)
            if data is None:
                return b''
            return data[offset:offset + size]

        def write(self, path, data, offset, fh):
            name = path.lstrip('/')
            old = driver.read_file(name) or b''
            new = old[:offset] + data + old[offset + len(data):]
            driver.write_file(name, new)
            return len(data)

        def truncate(self, path, length, fh=None):
            name = path.lstrip('/')
            old = driver.read_file(name) or b''
            driver.write_file(name, old[:length])
            return 0

        def unlink(self, path):
            name = path.lstrip('/')
            driver.remove_file(name)
            return 0

    driver.start_sync()
    print(f"  Mounted at {mountpoint}")
    print(f"  Cache: {cache.cache_path}")
    print("  Press Ctrl+C to unmount")

    try:
        fuse.FUSE(TapeFS(), mountpoint, foreground=True, allow_other=False)
    except KeyboardInterrupt:
        pass
    finally:
        driver.stop_sync()


# ======================================================================
#  CLI
# ======================================================================

def main():
    parser = argparse.ArgumentParser(description="TapewormFS Host Driver")
    parser.add_argument('--port', default='stdio',
                        help='Serial port (/dev/ttyUSB0) or stdio for testing')
    parser.add_argument('--baud', type=int, default=115200)

    sub = parser.add_subparsers(dest='command', required=True)

    # mount
    m = sub.add_parser('mount', help='Mount tape as FUSE folder')
    m.add_argument('mountpoint', help='Where to mount (e.g. ./tape_mount)')

    # sync
    s = sub.add_parser('sync', help='Sync local folder to/from tape')
    s.add_argument('folder', help='Local folder to sync')

    # ls
    sub.add_parser('ls', help='List files on tape')

    # format
    sub.add_parser('format', help='Format the tape')

    # read
    r = sub.add_parser('read', help='Read a file from tape')
    r.add_argument('filename')
    r.add_argument('output', nargs='?', help='Output file (default: stdout)')

    # write
    w = sub.add_parser('write', help='Write a file to tape')
    w.add_argument('input', help='Local file to write')
    w.add_argument('filename', nargs='?', help='Name on tape (default: same as input)')

    args = parser.parse_args()

    # Connect to ESP32
    if args.port != 'stdio' and args.command != 'mount':
        # For mount, connection is handled by fuse_mount
        pass

    esp = Esp32Connection(args.port, args.baud)

    if args.command == 'mount':
        fuse_mount(args.mountpoint, esp)

    elif args.command == 'sync':
        cache = TapeCache()
        driver = HostDriver(esp, cache)

        # Read tape dir
        print(f"Syncing tape ↔ {args.folder} ...")
        for entry in driver.list_files():
            local_path = os.path.join(args.folder, entry['name'])
            print(f"  {entry['name']} ({entry['size']} B)")
            data = driver.read_file(entry['name'])
            if data:
                os.makedirs(os.path.dirname(local_path), exist_ok=True)
                with open(local_path, 'wb') as f:
                    f.write(data)

        # Write local files to tape
        for fname in os.listdir(args.folder):
            fpath = os.path.join(args.folder, fname)
            if os.path.isfile(fpath):
                with open(fpath, 'rb') as f:
                    data = f.read()
                print(f"  Writing '{fname}' ({len(data)} B) to tape...")
                driver.write_file(fname, data)

        driver._sync_all()
        print("  Done")

    elif args.command == 'ls':
        cache = TapeCache()
        driver = HostDriver(esp, cache)
        entries = driver.list_files()
        if not entries:
            print("  (empty tape)")
        for e in entries:
            print(f"  {e['name']:<20} {e['size']:>8} B  blocks {e['blocks']}")

    elif args.command == 'format':
        esp = Esp32Connection(args.port, args.baud)
        fs = esp.make_filesystem()
        if fs.format() == Error.OK:
            print("  Tape formatted")
        else:
            print("  Format failed")

    elif args.command == 'read':
        cache = TapeCache()
        driver = HostDriver(esp, cache)
        data = driver.read_file(args.filename)
        if data is None:
            print(f"  File '{args.filename}' not found", file=sys.stderr)
            sys.exit(1)
        if args.output:
            with open(args.output, 'wb') as f:
                f.write(data)
            print(f"  Wrote {len(data)} B to {args.output}")
        else:
            sys.stdout.buffer.write(data)

    elif args.command == 'write':
        local_path = args.input
        tape_name = args.filename or os.path.basename(local_path)
        with open(local_path, 'rb') as f:
            data = f.read()
        cache = TapeCache()
        driver = HostDriver(esp, cache)
        driver.write_file(tape_name, data)
        driver._sync_all()
        print(f"  Wrote '{tape_name}' ({len(data)} B) to tape")


if __name__ == "__main__":
    main()
