"""
TapewormFS — Store files on audio cassette tapes.

One file. Pure Python. No dependencies beyond the stdlib.

Architecture:
  tapefs.py          ← you are here (everything)
  test_tapefs.py     ← tests
  dummy_mcu.py       ← simulated ESP32 firmware for testing
"""

import struct
import random
import time
import io
from dataclasses import dataclass, field
from typing import Callable, Optional
from enum import IntEnum

# ======================================================================
#  Constants
# ======================================================================

BLOCK_HEADER    = 5     # type(1) + seq_no(4)
BLOCK_DATA_MAX  = 1000  # max payload bytes per block
ECC_PARITY      = 16    # RS(255,239) parity bytes
CRC_BYTES       = 4
BLOCK_SIZE_MAX  = BLOCK_HEADER + BLOCK_DATA_MAX + ECC_PARITY + CRC_BYTES

MAX_FILES       = 32
FILENAME_LEN    = 20
RS_DATA_BYTES   = 239   # RS(255,239) data chunk


# ======================================================================
#  Block Types & Error Codes
# ======================================================================

class BlockType(IntEnum):
    DATA      = 0x01
    DIRECTORY = 0x02
    FAT       = 0x03
    ECC       = 0x04
    EOT       = 0xFF

class Error(IntEnum):
    OK       = 0
    IO       = -1
    CRC      = -2
    ECC      = -3
    NOT_FOUND = -4
    FULL     = -5
    INVALID  = -6


# ======================================================================
#  CRC-32  (ISO 3309 / IEEE 802.3)
# ======================================================================

def _build_crc_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        table.append(crc)
    return table

_CRC_TABLE = _build_crc_table()

def crc32(data: bytes) -> int:
    """Return CRC-32 of *data*."""
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


# ======================================================================
#  RS(255,239) — Reed-Solomon Error Correction
# ======================================================================
#
#  This is the gnarly bit.  Skip to "Directory" if you just want the
#  high-level API.
#
#  RS(255,239) takes 239 bytes of data and produces 16 parity bytes.
#  It can correct up to 8 corrupted bytes per 255-byte codeword.
#
#  The math (GF(256) with polynomial 0x11D) is handled by lookup
#  tables.  Encoding is polynomial division.  Decoding is syndrome
#  calculation + Berlekamp-Massey + Chien search + Forney.

def _gf_init():
    """Build GF(256) log/antilog tables with primitive poly 0x11D."""
    log = [0] * 256
    alog = [0] * 256
    x = 1
    for i in range(255):
        alog[i] = x
        log[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
    log[0] = 0
    alog[255] = alog[0]
    return log, alog

_LOG, _ALOG = _gf_init()

def _gf_mul(a: int, b: int) -> int:
    """Multiply two bytes in GF(256)."""
    if a == 0 or b == 0:
        return 0
    return _ALOG[(_LOG[a] + _LOG[b]) % 255]

def _gf_div(a: int, b: int) -> int:
    """Divide a by b in GF(256)."""
    if a == 0 or b == 0:
        return 0
    d = _LOG[a] - _LOG[b]
    if d < 0:
        d += 255
    return _ALOG[d]

def _rs_gen_poly():
    """Build generator polynomial g(x) for RS(255,239).
    
    g(x) = (x + 2^0)(x + 2^1)...(x + 2^15)
    Returns list of 17 coefficients [g0, g1, ..., g16] where g16 = 1.
    """
    poly = [1]  # start with g(x) = 1
    for i in range(16):
        a = _ALOG[i]  # root = 2^i
        # Multiply: new_poly = old_poly * (x + a)
        #            = old_poly * x + a * old_poly
        new = [0] * (len(poly) + 1)
        for j, coeff in enumerate(poly):
            new[j + 1] ^= coeff                 # x * old
            new[j] ^= _gf_mul(coeff, a)         # a * old
        poly = new
    return poly  # 17 coefficients, poly[16] = 1

_RS_GEN = _rs_gen_poly()  # generator coefficients, g[16] = 1

def rs_encode(data: bytes) -> bytes:
    """
    Encode 239 bytes into 16 parity bytes.
    
    This is polynomial long division: divide data*x^16 by g(x),
    the remainder is the parity.
    """
    assert len(data) == RS_DATA_BYTES
    dividend = list(data) + [0] * 16  # pad with 16 zeros

    for i in range(RS_DATA_BYTES):
        if dividend[i]:
            fb = dividend[i]
            for j in range(17):
                dividend[i + 16 - j] ^= _gf_mul(fb, _RS_GEN[j])

    return bytes(dividend[RS_DATA_BYTES:])

def rs_decode(data: bytearray, parity: bytes) -> Error:
    """
    Attempt to correct errors in *data* using *parity*.
    
    Returns Error.OK if no errors (or corrected).
    Returns Error.ECC if too many errors to fix.
    
    NOTE: The decoder (BM + Chien + Forney) is complex and still
    being debugged. For now, if there are no errors it returns OK.
    If there ARE errors it *may* correct them, or may return ECC.
    The encode + no-error-decode path is reliable.
    """
    assert len(data) == RS_DATA_BYTES
    assert len(parity) == ECC_PARITY

    # Build full codeword
    cw = list(data) + list(parity)

    # ---- Syndromes ------------------------------------------------ #
    # A valid codeword has S_i = cw(α^i) = 0 for i = 0..15
    syn = []
    nerr = 0
    for i in range(16):
        s = 0
        for b in cw:
            s = _gf_mul(s, _ALOG[i]) ^ b
        syn.append(s)
        if s:
            nerr += 1

    if nerr == 0:
        return Error.OK

    # ---- Berlekamp-Massey ----------------------------------------- #
    # Find the error locator polynomial Lambda(x)
    lam = [1] + [0] * 16
    b = [1] + [0] * 16
    L = 0
    m = 1

    for r in range(16):
        # Discrepancy
        d = 0
        for j in range(L + 1):
            d ^= _gf_mul(lam[j], syn[r - j])

        if d == 0:
            m += 1
        else:
            t = lam[:]
            for j in range(17 - m):
                if b[j]:
                    lam[j + m] ^= _gf_mul(d, b[j])

            if 2 * L <= r:
                L = r + 1 - L
                b = t[:]
                dinv = _gf_div(1, d)
                for j in range(17):
                    b[j] = _gf_mul(b[j], dinv)
                m = 1
            else:
                m += 1

    # Omega(x) = S(x) * Lambda(x) mod x^16
    omega = [0] * 16
    for i in range(16):
        for j in range(i + 1):
            omega[i] ^= _gf_mul(syn[j], lam[i - j])

    if L == 0 or L > 8:
        return Error.ECC  # too many errors

    # ---- Chien search --------------------------------------------- #
    # Find roots of Lambda(x) = actual error positions
    positions = []
    for i in range(1, 256):
        val = 0
        apow = 1
        for j in range(L + 1):
            val ^= _gf_mul(lam[j], apow)
            apow = _gf_mul(apow, _ALOG[1])
        if val == 0:
            positions.append(255 - i)
            if len(positions) >= L:
                break

    if len(positions) != L:
        return Error.ECC

    # ---- Forney ---------------------------------------------------- #
    # Compute error values at each position
    values = [0] * L
    for i, pos in enumerate(positions):
        X = 254 - pos
        Xi = _ALOG[(255 - _LOG[_ALOG[X]]) % 255]

        # Omega(X⁻¹)
        omegaX = 0
        apow = 1
        for j in range(L):
            omegaX ^= _gf_mul(omega[j], apow)
            apow = _gf_mul(apow, Xi)

        # Lambda'(X⁻¹)  — formal derivative
        lambdaX = 0
        for j in range(1, L + 1, 2):
            lambdaX ^= _gf_mul(lam[j], _ALOG[(j * _LOG[Xi]) % 255])

        values[i] = _gf_div(omegaX, lambdaX) if lambdaX else 0

    # Correct errors
    for pos, val in zip(positions, values):
        if pos < RS_DATA_BYTES:
            data[pos] ^= val

    return Error.OK


# ======================================================================
#  Directory
# ======================================================================
#
#  Stored as a block payload near the start of tape.
#  Wire format:
#    [0..2]  "TWF"       (magic)
#    [3]     file_count  (1 byte)
#    [4..]   entries     (each 32 bytes)

def _pack_u32(v: int) -> bytes:
    return struct.pack('<I', v)

def _unpack_u32(buf: bytes, offset: int) -> int:
    return struct.unpack_from('<I', buf, offset)[0]

@dataclass
class DirEntry:
    filename: str = ""
    start_block: int = 0
    end_block: int = 0
    file_size: int = 0

class Directory:
    """Tape directory — a list of files."""

    def __init__(self):
        self.entries: list[DirEntry] = []

    def add(self, filename: str, file_size: int,
            start_block: int, end_block: int) -> Error:
        """Add a file entry. Returns FULL if at max capacity."""
        if len(self.entries) >= MAX_FILES:
            return Error.FULL
        # Duplicate check
        for e in self.entries:
            if e.filename == filename:
                return Error.INVALID
        entry = DirEntry(
            filename=filename[:FILENAME_LEN],
            file_size=file_size,
            start_block=start_block,
            end_block=end_block,
        )
        self.entries.append(entry)
        return Error.OK

    def find(self, filename: str) -> Optional[DirEntry]:
        """Find a file by name, or None."""
        for e in self.entries:
            if e.filename == filename:
                return e
        return None

    def remove(self, filename: str) -> Error:
        """Remove a file by name."""
        for i, e in enumerate(self.entries):
            if e.filename == filename:
                self.entries.pop(i)
                return Error.OK
        return Error.NOT_FOUND

    @property
    def count(self) -> int:
        return len(self.entries)

    def serialise(self) -> bytes:
        """Convert directory to raw bytes (for a block payload)."""
        buf = bytearray()
        buf.extend(b'TWF')
        buf.append(len(self.entries))
        for e in self.entries:
            # Filename padded to 20 bytes
            name = e.filename.encode('ascii', errors='replace')
            name = name[:FILENAME_LEN].ljust(FILENAME_LEN, b'\x00')
            buf.extend(name)
            buf.extend(_pack_u32(e.start_block))
            buf.extend(_pack_u32(e.end_block))
            buf.extend(_pack_u32(e.file_size))
        return bytes(buf)

    @staticmethod
    def deserialise(data: bytes) -> 'Directory':
        """Parse raw bytes into a Directory."""
        dir_ = Directory()
        if len(data) < 4:
            return dir_
        if data[:3] != b'TWF':
            return dir_

        count = min(data[3], MAX_FILES)
        pos = 4
        for _ in range(count):
            if pos + 32 > len(data):
                break
            e = DirEntry()
            # Filename
            raw_name = data[pos:pos + FILENAME_LEN]
            e.filename = raw_name.rstrip(b'\x00').decode('ascii', errors='replace')
            pos += FILENAME_LEN
            # Numbers
            e.start_block = _unpack_u32(data, pos); pos += 4
            e.end_block   = _unpack_u32(data, pos); pos += 4
            e.file_size   = _unpack_u32(data, pos); pos += 4
            dir_.entries.append(e)

        return dir_


# ======================================================================
#  Block Serialisation
# ======================================================================
#
#  Before writing to the transport layer, a block gets RS parity
#  and a CRC-32 tag.  Reading strips them and verifies integrity.

@dataclass
class Block:
    """A logical block (before RS + CRC)."""
    type: BlockType = BlockType.DATA
    seq_no: int = 0
    data: bytes = b''

@dataclass
class RawBlock:
    """A serialised block (RS parity + CRC appended)."""
    bytes: bytes = b''

def serialise(block: Block) -> RawBlock:
    """
    Add RS parity and CRC to a block.
    
    1. Build header (type + seq_no)
    2. Append payload
    3. RS-encode in 239-byte chunks → append parity
    4. CRC-32 over everything → append
    """
    buf = bytearray()

    # Header
    buf.append(block.type)
    buf.extend(_pack_u32(block.seq_no))

    # Payload (capped at max size)
    payload = block.data[:BLOCK_DATA_MAX]
    buf.extend(payload)

    # RS parity — process header+payload in 239-byte chunks
    total = BLOCK_HEADER + len(payload)
    offset = 0
    while offset < total:
        chunk = buf[offset:offset + RS_DATA_BYTES]
        # Pad with zeros to 239 bytes
        chunk = chunk.ljust(RS_DATA_BYTES, b'\x00')
        parity = rs_encode(bytes(chunk))
        buf.extend(parity)
        offset += RS_DATA_BYTES

    # CRC-32
    crc = crc32(bytes(buf))
    buf.extend(_pack_u32(crc))

    return RawBlock(bytes=bytes(buf))

def deserialise(raw: RawBlock) -> tuple[Block, Error]:
    """
    Strip RS parity and CRC, verify integrity.
    
    Returns (block, Error.OK) on success.
    Returns (Block(), Error.CRC) if CRC is bad.
    Returns (Block(), Error.INVALID) if malformed.
    """
    buf = raw.bytes
    if len(buf) < BLOCK_HEADER + CRC_BYTES:
        return Block(), Error.INVALID

    # Verify CRC
    crc_data = buf[:-CRC_BYTES]
    stored_crc = _unpack_u32(buf, len(buf) - CRC_BYTES)
    if crc32(crc_data) != stored_crc:
        return Block(), Error.CRC

    # Parse header
    block = Block(
        type=BlockType(buf[0]),
        seq_no=_unpack_u32(buf, 1),
    )

    # Find payload boundaries — try chunk counts until one fits
    found = False
    for chunks in range(BLOCK_SIZE_MAX // ECC_PARITY + 1):
        parity_bytes = chunks * ECC_PARITY
        payload_end = len(buf) - CRC_BYTES - parity_bytes
        if payload_end < BLOCK_HEADER:
            break

        data_len = payload_end - BLOCK_HEADER
        if data_len > BLOCK_DATA_MAX:
            continue

        # Does this chunk count match?
        total = BLOCK_HEADER + data_len
        expected = (total + RS_DATA_BYTES - 1) // RS_DATA_BYTES
        if expected < 1 and data_len > 0:
            expected = 1

        if chunks != expected and not (data_len == 0 and chunks == 0):
            continue

        # Extract payload
        payload = buf[BLOCK_HEADER:payload_end]
        block.data = payload

        # RS decode each chunk (correct errors where possible)
        parity_ptr = payload_end
        offset = 0
        for _ in range(chunks):
            chunk_size = min(total - offset, RS_DATA_BYTES)
            chunk = bytearray(buf[offset:offset + chunk_size])
            chunk_data = chunk.ljust(RS_DATA_BYTES, b'\x00')
            chunk_parity = buf[parity_ptr:parity_ptr + ECC_PARITY]

            rs_decode(chunk_data, chunk_parity)

            # Write corrected data back
            data_arr = bytearray(block.data)
            for j in range(chunk_size):
                global_pos = offset + j
                if global_pos >= BLOCK_HEADER:
                    idx = global_pos - BLOCK_HEADER
                    if idx < len(data_arr):
                        data_arr[idx] = chunk_data[j]
            block.data = bytes(data_arr)

            parity_ptr += ECC_PARITY
            offset += chunk_size

        found = True
        break

    if not found:
        return Block(), Error.INVALID

    return block, Error.OK


# ======================================================================
#  Filesystem — High-Level API
# ======================================================================

# Transport callbacks — how the filesystem talks to the MCU
WriteFn = Callable[[RawBlock], bool]
ReadFn  = Callable[[], Optional[RawBlock]]
SeekFn  = Callable[[int], bool]   # seek to block number

class Filesystem:
    """
    High-level filesystem operations.
    
    Usage:
        def my_write(block): ...  # send block to MCU
        def my_read(): ...        # read block from MCU
        def my_seek(n): ...       # position tape head
        
        fs = Filesystem(my_write, my_read, my_seek)
        fs.format()
        fs.write_file("hello.txt", b"Hello Tape!")
        data = fs.read_file("hello.txt")
    """

    def __init__(self, write_fn: WriteFn, read_fn: ReadFn, seek_fn: SeekFn):
        self._write = write_fn
        self._read  = read_fn
        self._seek  = seek_fn
        self._next_block = 0  # next free block for writing

    # ---- Format --------------------------------------------------- #

    def format(self) -> Error:
        """Erase tape: write empty directory + EOT marker."""
        if not self._seek(0):
            return Error.IO

        # Empty directory
        dir_ = Directory()
        raw = serialise(Block(type=BlockType.DIRECTORY, data=dir_.serialise()))
        if not self._write(raw):
            return Error.IO

        # EOT marker
        raw = serialise(Block(type=BlockType.EOT, seq_no=1))
        if not self._write(raw):
            return Error.IO

        self._next_block = 2
        return Error.OK

    # ---- Directory ------------------------------------------------ #

    def read_directory(self) -> tuple[Directory, Error]:
        """Rewind and read directory from tape."""
        if not self._seek(0):
            return Directory(), Error.IO
        raw = self._read()
        if raw is None:
            return Directory(), Error.IO

        block, err = deserialise(raw)
        if err != Error.OK:
            return Directory(), err
        if block.type != BlockType.DIRECTORY:
            return Directory(), Error.INVALID

        dir_ = Directory.deserialise(block.data)
        return dir_, Error.OK

    def write_directory(self, dir_: Directory) -> Error:
        """Overwrite directory on tape (block 0)."""
        if not self._seek(0):
            return Error.IO
        raw = serialise(Block(type=BlockType.DIRECTORY, data=dir_.serialise()))
        return Error.OK if self._write(raw) else Error.IO

    # ---- File Write ----------------------------------------------- #

    def write_file(self, filename: str, data: bytes) -> Error:
        """
        Write a file to tape.
        
        Splits data into blocks, writes sequentially, then updates
        the directory.  Returns Error.OK on success.
        """
        # Read current directory
        dir_, err = self.read_directory()
        if err not in (Error.OK, Error.CRC, Error.ECC):
            dir_ = Directory()  # start fresh

        if dir_.find(filename):
            return Error.INVALID  # already exists

        # Seek to first available data block
        start_block = self._next_block
        if start_block < 1:
            start_block = 1

        if not self._seek(start_block):
            return Error.IO

        # Write data blocks
        seq = start_block
        offset = 0
        while offset < len(data):
            chunk = data[offset:offset + BLOCK_DATA_MAX]
            block = Block(type=BlockType.DATA, seq_no=seq, data=chunk)
            raw = serialise(block)
            if not self._write(raw):
                return Error.IO
            offset += BLOCK_DATA_MAX
            seq += 1

        # Write EOT marker
        raw = serialise(Block(type=BlockType.EOT, seq_no=seq))
        if not self._write(raw):
            return Error.IO

        # Update directory
        end_block = seq - 1
        err = dir_.add(filename, len(data), start_block, end_block)
        if err != Error.OK:
            return err

        err = self.write_directory(dir_)
        if err != Error.OK:
            return err

        self._next_block = seq + 1
        return Error.OK

    # ---- File Read ------------------------------------------------ #

    def read_file(self, filename: str) -> tuple[bytes, Error]:
        """
        Read a file from tape.
        
        Returns (data, Error.OK) on success.
        Returns (b'', Error.NOT_FOUND) if missing.
        """
        # Read directory
        dir_, err = self.read_directory()
        if err != Error.OK:
            return b'', err

        entry = dir_.find(filename)
        if entry is None:
            return b'', Error.NOT_FOUND

        # Seek to start of file
        if not self._seek(entry.start_block):
            return b'', Error.IO

        # Read blocks
        result = bytearray()
        for seq in range(entry.start_block, entry.end_block + 1):
            raw = self._read()
            if raw is None:
                break

            block, err2 = deserialise(raw)
            if err2 not in (Error.OK, Error.CRC, Error.ECC):
                continue  # skip corrupt blocks

            if block.type == BlockType.EOT:
                break
            if block.type != BlockType.DATA:
                continue

            result.extend(block.data)

        return bytes(result), Error.OK

    # ---- List / Stat ---------------------------------------------- #

    def list_files(self) -> tuple[Directory, Error]:
        """List all files on tape."""
        return self.read_directory()

    def stat(self) -> tuple[int, int, int]:
        """
        Return (total_blocks, used_blocks, free_blocks).
        
        Estimates based on a typical C60 cassette (~360 blocks).
        """
        total = 360
        used  = self._next_block
        free  = total - used
        return total, used, free
