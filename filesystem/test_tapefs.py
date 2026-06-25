"""
Tests for tapefs.py — pure Python filesystem on tape cassette.

Run with:  python3 test_tapefs.py
"""

import tapefs
from tapefs import (
    Block, RawBlock, Directory, DirEntry, Filesystem,
    BlockType, Error,
    serialise, deserialise,
    crc32, rs_encode, rs_decode,
)

# ======================================================================
#  Mock transport — simulates a tape in memory
# ======================================================================

class MockTape:
    """An in-memory tape that the filesystem driver talks to."""

    def __init__(self):
        self.blocks: list[RawBlock] = []
        self.pos = 0

    def write(self, block: RawBlock) -> bool:
        if self.pos >= 256:
            return False
        if self.pos >= len(self.blocks):
            self.blocks.append(block)
        else:
            self.blocks[self.pos] = block
        self.pos += 1
        return True

    def read(self) -> RawBlock | None:
        if self.pos >= len(self.blocks):
            return None
        result = self.blocks[self.pos]
        self.pos += 1
        return result

    def seek(self, block_no: int) -> bool:
        self.pos = block_no
        return True


def test_crc32():
    """CRC-32 is deterministic and different for different data."""
    c1 = crc32(b"Hello TapewormFS!")
    c2 = crc32(b"Hello TapewormFS!")
    c3 = crc32(b"Hello TapewormFS?")

    assert c1 == c2, "CRC should be deterministic"
    assert c1 != c3, "Different data → different CRC"
    print("  CRC32... OK")


def test_rs_encode():
    """RS encode produces 16 parity bytes from 239 data bytes."""
    data = bytes(range(239))
    parity = rs_encode(data)

    assert len(parity) == 16, f"Expected 16 parity bytes, got {len(parity)}"

    # Codeword should have zero syndromes
    cw = data + parity
    from tapefs import _ALOG, _gf_mul
    all_ok = True
    for i in range(16):
        s = 0
        for b in cw:
            s = _gf_mul(s, _ALOG[i]) ^ b
        if s:
            all_ok = False
            break
    assert all_ok, "Syndromes should be zero for valid codeword"

    # Decode with no errors should succeed
    data_arr = bytearray(data)
    err = rs_decode(data_arr, parity)
    assert err == Error.OK, f"No-error decode should return OK, got {err}"

    print("  RS ECC encode/decode (no errors)... OK")


def test_block_serialise():
    """Block round-trips through serialise/deserialise."""
    block = Block(type=BlockType.DATA, seq_no=42, data=b"Hello TapeBlock")
    raw = serialise(block)
    assert len(raw.bytes) > 0
    assert len(raw.bytes) <= tapefs.BLOCK_SIZE_MAX

    block2, err = deserialise(raw)
    assert err == Error.OK
    assert block2.type == BlockType.DATA
    assert block2.seq_no == 42
    assert block2.data == b"Hello TapeBlock"

    # Corrupted bytes → CRC error
    buf = bytearray(raw.bytes)
    buf[10] ^= 0xFF
    _, err = deserialise(RawBlock(bytes=bytes(buf)))
    assert err == Error.CRC, "Corrupted block should give CRC error"

    print("  Block serialise/deserialise... OK")


def test_directory():
    """Directory add/find/remove/serialise round-trip."""
    dir_ = Directory()
    assert dir_.count == 0

    # Add files
    assert dir_.add("readme.txt", 1024, 5, 6) == Error.OK
    assert dir_.count == 1
    assert dir_.add("data.bin", 4096, 7, 10) == Error.OK
    assert dir_.count == 2

    # Find
    e = dir_.find("readme.txt")
    assert e is not None
    assert e.file_size == 1024
    assert e.start_block == 5

    assert dir_.find("nonexistent") is None

    # Duplicate
    assert dir_.add("readme.txt", 0, 0, 0) == Error.INVALID

    # Remove
    assert dir_.remove("readme.txt") == Error.OK
    assert dir_.count == 1
    assert dir_.find("readme.txt") is None

    # Serialise round-trip
    dir2 = Directory()
    dir2.add("file.a", 100, 1, 1)
    dir2.add("file.b", 200, 2, 3)

    data = dir2.serialise()
    dir3 = Directory.deserialise(data)
    assert dir3.count == 2
    assert dir3.find("file.a") is not None
    assert dir3.find("file.a").file_size == 100

    print("  Directory operations... OK")


def test_write_read_file():
    """Integration test: write a file to mock tape, read it back."""
    tape = MockTape()
    fs = Filesystem(tape.write, tape.read, tape.seek)

    # Format
    assert fs.format() == Error.OK

    # Write
    content = b"This is test data for TapewormFS! " \
              b"The quick brown fox jumps over the lazy dog."
    err = fs.write_file("test.txt", content)
    assert err == Error.OK, f"Write failed: {err}"

    # Read back
    data, err = fs.read_file("test.txt")
    assert err == Error.OK, f"Read failed: {err}"
    assert data == content, f"Data mismatch: {len(data)} vs {len(content)}"

    # List
    dir_, err = fs.list_files()
    assert err == Error.OK
    assert dir_.count == 1
    assert dir_.entries[0].filename == "test.txt"
    assert dir_.entries[0].file_size == len(content)

    # File not found
    data, err = fs.read_file("nonexistent.txt")
    assert err == Error.NOT_FOUND

    print("  Write/read file via mock tape... OK")


def test_multiple_files():
    """Write multiple files, verify directory tracks all."""
    tape = MockTape()
    fs = Filesystem(tape.write, tape.read, tape.seek)
    fs.format()

    files = {
        "a.txt": b"Hello A",
        "b.bin": b"Binary data " * 10,
        "longer_name.txt": b"X" * 500,
    }

    for name, data in files.items():
        err = fs.write_file(name, data)
        assert err == Error.OK, f"Write {name} failed: {err}"

    # Read back each
    for name, expected in files.items():
        data, err = fs.read_file(name)
        assert err == Error.OK, f"Read {name} failed: {err}"
        assert data == expected, f"{name}: {len(data)} vs {len(expected)}"

    # List should show all
    dir_, err = fs.list_files()
    assert err == Error.OK
    assert dir_.count == len(files)

    print("  Multiple files... OK")


if __name__ == "__main__":
    print("TapewormFS Filesystem Tests (Python)")
    print("=" * 40)
    print()

    test_crc32()
    test_rs_encode()
    test_block_serialise()
    test_directory()
    test_write_read_file()
    test_multiple_files()

    print()
    print("All tests passed!")
