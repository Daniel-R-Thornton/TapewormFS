"""
DummyMCU — Simulated ESP32/RP2040 firmware for testing.

Emulates the real cassette hardware so you can test the filesystem
without a physical tape deck.  Blocks are stored in memory.

Three modes:
  1. Direct class — wire into Filesystem callbacks for unit tests
  2. TCP server  — run standalone, connect with anything
  3. Stdio       — binary packets on stdin/stdout (scripting/debug)

Usage:
    from dummy_mcu import DummyMCU
    mcu = DummyMCU()
    fs  = Filesystem(mcu.write_callback, mcu.read_callback, mcu.seek_callback)
"""

import struct
import time
import random
import sys
import threading
from enum import IntEnum
from typing import Optional

# Import the tapefs types
from tapefs import RawBlock, Error, ECC_PARITY, CRC_BYTES, crc32

# ======================================================================
#  Protocol Constants (from SPEC.md §3)
# ======================================================================

START_MARKER = 0xFE

# Command IDs (host → MCU)
CMD_PING        = 0x01
CMD_GET_STATUS  = 0x02
CMD_SEEK        = 0x03
CMD_REWIND      = 0x04
CMD_WRITE_BLOCK = 0x05
CMD_READ_NEXT   = 0x06
CMD_FLUSH       = 0x07
CMD_STOP        = 0x08
CMD_SET_CONFIG  = 0x09
CMD_STREAM_READ = 0x0A
CMD_STREAM_WRITE = 0x0B

# Response IDs (MCU → host) — request_id | 0x80
RSP_PING        = 0x81
RSP_GET_STATUS  = 0x82
RSP_SEEK        = 0x83
RSP_REWIND      = 0x84
RSP_WRITE_BLOCK = 0x85
RSP_READ_NEXT   = 0x86
RSP_FLUSH       = 0x87
RSP_STOP        = 0x88
RSP_SET_CONFIG  = 0x89
RSP_STREAM_READ = 0x8A
RSP_STREAM_WRITE = 0x8B

# Unsolicited events (MCU → host, spontaneous)
EVT_STATUS_CHANGE   = 0xC0
EVT_BLOCK_READ      = 0xC1
EVT_READ_ERROR      = 0xC2
EVT_PROGRESS        = 0xC3
EVT_BLOCK_WRITTEN   = 0xC4

# Error codes (1-byte NAK payload)
ERR_UNKNOWN_CMD    = 0x01
ERR_INVALID_STATE  = 0x02
ERR_CHECKSUM       = 0x03
ERR_INVALID_PARAM  = 0x04
ERR_BUFFER_FULL    = 0x05
ERR_TIMEOUT        = 0x06
ERR_NO_TAPE        = 0x07
ERR_WRITE_PROTECT  = 0x08
ERR_BLOCK_CRC      = 0x09
ERR_ECC_FAILED     = 0x0A
ERR_HW_FAULT       = 0x0B

# Status flag bits
FLAG_TAPE_PRESENT   = 1 << 0
FLAG_TAPE_MOVING    = 1 << 1
FLAG_WRITE_PROTECT  = 1 << 2
FLAG_STATE_BUSY     = 1 << 3
FLAG_BUFFER_EMPTY   = 1 << 4
FLAG_BUFFER_FULL    = 1 << 5
FLAG_ERROR          = 1 << 6
FLAG_EOT            = 1 << 7
FLAG_BOT            = 1 << 8
FLAG_POSITION_LOCKED = 1 << 9
FLAG_CRC_MISMATCH   = 1 << 10
FLAG_OVERRUN        = 1 << 11

# ======================================================================
#  Packet Layer — Frame / Deframe / CRC-16
# ======================================================================

def _crc16(data: bytes) -> int:
    """CRC-16-IBM (0x8005, init 0x0000)."""
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

def _escape(data: bytes) -> bytes:
    """Escape 0xFE → 0xFD 0x01 and 0xFD → 0xFD 0x02."""
    result = bytearray()
    for b in data:
        if b == 0xFE:
            result.extend([0xFD, 0x01])
        elif b == 0xFD:
            result.extend([0xFD, 0x02])
        else:
            result.append(b)
    return bytes(result)

def _unescape(data: bytes) -> bytes:
    """Reverse escaping."""
    result = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFD:
            if i + 1 < len(data):
                if data[i + 1] == 0x01:
                    result.append(0xFE)
                elif data[i + 1] == 0x02:
                    result.append(0xFD)
                i += 2
                continue
            else:
                break  # stray FD at end
        else:
            result.append(data[i])
            i += 1
    return bytes(result)

def encode_packet(cmd_id: int, payload: bytes = b'') -> bytes:
    """
    Build a framed packet ready to send.
    
    Format: [0xFE | len(2B) | cmd_id(1B) | payload(N) | crc16(2B)]
    The len field, payload, and CRC are escaped.
    """
    inner = struct.pack('<H', 1 + len(payload) + 2)  # cmd_id + payload + crc
    inner += bytes([cmd_id])
    inner += payload

    crc = _crc16(inner)
    inner += struct.pack('<H', crc)

    escaped = _escape(inner)
    return bytes([START_MARKER]) + escaped

def decode_packet(data: bytes) -> Optional[dict]:
    """
    Parse a framed packet.
    
    Returns dict with 'cmd_id', 'payload', or None if invalid.
    """
    if not data:
        return None
    if data[0] != START_MARKER:
        return None

    inner = _unescape(data[1:])
    if len(inner) < 5:  # len(2) + cmd_id(1) + crc(2)
        return None

    length = struct.unpack_from('<H', inner, 0)[0]
    if len(inner) < 2 + length:
        return None  # truncated

    cmd_id = inner[2]
    payload_len = length - 1 - 2  # subtract cmd_id(1) + crc(2)
    payload = inner[3:3 + payload_len]
    stored_crc = struct.unpack_from('<H', inner, 3 + payload_len)[0]

    # Verify CRC
    computed = _crc16(inner[:2 + length - 2])  # everything except CRC
    if computed != stored_crc:
        return None

    return {'cmd_id': cmd_id, 'payload': payload}


# ======================================================================
#  State Machine
# ======================================================================

class State(IntEnum):
    IDLE    = 0
    SEEK    = 1
    REWIND  = 2
    READ    = 3
    WRITE   = 4
    STREAM  = 5
    ERROR   = 6


# ======================================================================
#  DummyMCU
# ======================================================================

class DummyMCU:
    """
    Software-emulated cassette firmware.
    
    Stores blocks in memory, simulates latency, speaks the transport
    protocol.  Use with Filesystem for end-to-end tests without hardware.
    
    Basic usage:
        mcu = DummyMCU()
        fs  = Filesystem(mcu.write, mcu.read, mcu.seek)
    
    Standalone TCP server:
        mcu = DummyMCU()
        mcu.run_tcp(port=9725)
    
    Stdio mode:
        mcu = DummyMCU()
        mcu.run_stdio()
    """

    def __init__(self, initial_tape: Optional[list[RawBlock]] = None):
        # Tape storage
        self._tape: list[RawBlock] = list(initial_tape) if initial_tape else []

        # State machine
        self._state = State.IDLE
        self._pos = 0  # current block position on tape
        self._write_buffer: list[RawBlock] = []  # buffered writes

        # Simulated flags
        self._tape_present = True
        self._write_protect = False
        self._bot = True      # at beginning of tape?
        self._eot = False     # at end of tape?
        self._pos_locked = True  # always locked in sim
        self._crc_error = False

        # Config (from SET_CONFIG)
        self._config = {
            'baud_rate': 200,
            'modulation': 0,
            'frame_size': 1024,
            'volume': 128,
            'agc_target': 128,
        }

        # Simulated delays (milliseconds)
        self._delay_write = 10
        self._delay_read = 10
        self._delay_seek_per_block = 5
        self._delay_rewind = 500
        self._delay_flush = 20

        # Error injection
        self._error_crc_rate = 0    # inject CRC error every N ops (0 = off)
        self._error_drop_rate = 0   # drop response every N ops
        self._error_stall_ms = 0    # extra random stall
        self._error_no_tape = False
        self._error_write_protect = False
        self._op_count = 0

        # For transport callbacks (used by Filesystem)
        self._incoming: Optional[RawBlock] = None  # last command
        self._outgoing: Optional[RawBlock] = None  # next response

    # ---- Raw block callbacks (for Filesystem) -------------------- #
    # These bypass the packet protocol — they store/retrieve blocks
    # directly, as if talking to a raw tape device.

    def raw_write(self, block: RawBlock) -> bool:
        """Store a raw block on tape."""
        if self._error_no_tape or self._error_write_protect:
            return False
        if self._pos >= len(self._tape):
            self._tape.append(block)
        else:
            self._tape[self._pos] = block
        self._pos += 1
        return True

    def raw_read(self) -> Optional[RawBlock]:
        """Read the next raw block from tape."""
        if self._pos >= len(self._tape):
            return None
        result = self._tape[self._pos]
        self._pos += 1
        return result

    def raw_seek(self, block_no: int) -> bool:
        """Position the tape head at a given block."""
        if self._error_no_tape:
            return False
        blocks = abs(block_no - self._pos)
        delay = blocks * self._delay_seek_per_block
        if delay > 0:
            time.sleep(delay / 1000)
        self._pos = max(0, min(block_no, len(self._tape)))
        self._bot = (self._pos == 0)
        self._eot = (self._pos >= len(self._tape))
        return True

    # ---- Packet protocol callbacks -------------------------------- #
    # These handle the framed transport protocol (0xFE, CRC-16, etc.)

    def write_callback(self, block: RawBlock) -> bool:
        """
        Send a framed command packet to the MCU.
        Response is stored internally for read_callback().
        """
        if self._error_no_tape:
            self._outgoing = RawBlock(bytes=self._nak(ERR_NO_TAPE))
            return True

        self._incoming = block
        response = self._process_command(block.bytes)
        self._outgoing = RawBlock(bytes=response) if response else None
        return True

    def read_callback(self) -> Optional[RawBlock]:
        """Read the MCU's response to the last command."""
        result = self._outgoing
        self._outgoing = None
        if result is None:
            return None
        if self._error_drop_rate > 0:
            self._op_count += 1
            if self._op_count % self._error_drop_rate == 0:
                return None
        return result

    def seek_callback(self, block_no: int) -> bool:
        """Seek via packet protocol."""
        return self.raw_seek(block_no)

    # ---- Command Processing ---------------------------------------- #

    def _process_command(self, data: bytes) -> Optional[bytes]:
        """Parse and execute one transport command."""
        pkt = decode_packet(data)
        if pkt is None:
            return self._nak(ERR_CHECKSUM)

        cmd = pkt['cmd_id']
        payload = pkt['payload']

        # Build a response packet
        def respond(rsp_id: int, rsp_payload: bytes = b'') -> bytes:
            return encode_packet(rsp_id, rsp_payload)

        def nak(code: int) -> bytes:
            return encode_packet(cmd | 0x80, bytes([code]))

        # ---- PING ------------------------------------------------- #
        if cmd == CMD_PING:
            version = b"TapewormFS DummyMCU v0.1\x00"
            return respond(RSP_PING, version)

        # ---- GET_STATUS -------------------------------------------- #
        if cmd == CMD_GET_STATUS:
            # Tape position (14 bytes)
            pos = struct.pack('<II', self._pos, 0)   # block_number, byte_offset
            pos += struct.pack('<I', self._pos * 1000)  # tape_ms (1s per block)
            pos += bytes([0, 255])  # side=A, confidence=255

            # Status flags (2 bytes)
            flags = FLAG_TAPE_PRESENT | FLAG_POSITION_LOCKED
            if self._state != State.IDLE:
                flags |= FLAG_STATE_BUSY | FLAG_TAPE_MOVING
            if self._bot:
                flags |= FLAG_BOT
            if self._eot or self._pos >= len(self._tape):
                flags |= FLAG_EOT
            if self._write_protect or self._error_write_protect:
                flags |= FLAG_WRITE_PROTECT
            if self._crc_error:
                flags |= FLAG_CRC_MISMATCH
            if self._state == State.ERROR:
                flags |= FLAG_ERROR

            # Buffer level (1 byte)
            buf_level = min(255, len(self._write_buffer) * 20)

            return respond(RSP_GET_STATUS, pos + struct.pack('<H', flags) + bytes([buf_level]))

        # ---- SEEK ------------------------------------------------- #
        if cmd == CMD_SEEK:
            if len(payload) < 14:
                return nak(ERR_INVALID_PARAM)
            target = struct.unpack_from('<I', payload, 0)[0]
            if self._state not in (State.IDLE, State.READ, State.WRITE, State.STREAM):
                return nak(ERR_INVALID_STATE)

            self._state = State.SEEK
            blocks_to_move = abs(target - self._pos)
            delay = blocks_to_move * self._delay_seek_per_block
            if delay > 0:
                time.sleep(delay / 1000)

            self._pos = max(0, min(target, len(self._tape)))
            self._bot = (self._pos == 0)
            self._eot = (self._pos >= len(self._tape))
            self._state = State.IDLE
            return respond(RSP_SEEK, bytes([0]))  # ACK

        # ---- REWIND ----------------------------------------------- #
        if cmd == CMD_REWIND:
            if self._state not in (State.IDLE, State.READ, State.WRITE):
                return nak(ERR_INVALID_STATE)

            self._state = State.REWIND
            if self._delay_rewind > 0:
                time.sleep(self._delay_rewind / 1000)

            self._pos = 0
            self._bot = True
            self._eot = False
            self._state = State.IDLE
            return respond(RSP_REWIND, bytes([0]))

        # ---- WRITE_BLOCK ------------------------------------------- #
        if cmd == CMD_WRITE_BLOCK:
            if self._error_write_protect:
                return nak(ERR_WRITE_PROTECT)
            if self._state not in (State.IDLE, State.WRITE):
                return nak(ERR_INVALID_STATE)

            if len(payload) < 1:
                return nak(ERR_INVALID_PARAM)

            self._state = State.WRITE

            # Store the full payload (type + data) as the raw block
            rb = RawBlock(bytes=payload)
            self._write_buffer.append(rb)

            # Simulate write delay (only for first few blocks to be "on tape")
            # In real life, blocks are buffered and written asynchronously
            if self._delay_write > 0 and len(self._write_buffer) <= 3:
                time.sleep(self._delay_write / 1000)

            return respond(RSP_WRITE_BLOCK, bytes([0]))

        # ---- READ_NEXT --------------------------------------------- #
        if cmd == CMD_READ_NEXT:
            if self._state not in (State.IDLE, State.READ):
                return nak(ERR_INVALID_STATE)

            self._state = State.READ

            # Check if we're past end of tape
            if self._pos >= len(self._tape):
                return nak(ERR_NO_TAPE)  # "no more blocks"

            # Simulate read delay
            if self._delay_read > 0:
                time.sleep(self._delay_read / 1000)

            block = self._tape[self._pos]
            self._pos += 1
            self._bot = (self._pos == 0)
            self._eot = (self._pos >= len(self._tape))

            # Inject CRC error if configured
            data = block.bytes
            if self._error_crc_rate > 0:
                self._op_count += 1
                if self._op_count % self._error_crc_rate == 0:
                    buf = bytearray(data)
                    buf[-5] ^= 0xFF  # flip a CRC byte
                    data = bytes(buf)

            # Response: block_type(1) + block_data(N) + crc32(4)
            # block_data = data[1:] (strip the type byte)
            block_type = data[0] if data else 0xFF
            block_data = data[1:] if len(data) > 1 else b''
            rsp = bytes([block_type]) + block_data
            # Include CRC-32 of the block data at the end
            rsp += struct.pack('<I', crc32(block_data))
            return respond(RSP_READ_NEXT, rsp)

        # ---- FLUSH ------------------------------------------------- #
        if cmd == CMD_FLUSH:
            if self._state != State.WRITE:
                return nak(ERR_INVALID_STATE)

            # "Write" all buffered blocks to tape
            if self._delay_flush > 0:
                time.sleep(self._delay_flush / 1000)

            for rb in self._write_buffer:
                self._tape.append(rb)
            self._write_buffer.clear()

            self._state = State.IDLE
            return respond(RSP_FLUSH, bytes([0]))

        # ---- STOP -------------------------------------------------- #
        if cmd == CMD_STOP:
            # Lose any buffered writes
            self._write_buffer.clear()
            self._state = State.IDLE
            return respond(RSP_STOP, bytes([0]))

        # ---- SET_CONFIG -------------------------------------------- #
        if cmd == CMD_SET_CONFIG:
            if self._state != State.IDLE:
                return nak(ERR_INVALID_STATE)
            if len(payload) < 2:
                return nak(ERR_INVALID_PARAM)

            key = payload[0]
            if key == 0x01 and len(payload) >= 5:
                self._config['baud_rate'] = struct.unpack_from('<I', payload, 1)[0]
            elif key == 0x02 and len(payload) >= 2:
                self._config['modulation'] = payload[1]
            elif key == 0x03 and len(payload) >= 3:
                self._config['frame_size'] = struct.unpack_from('<H', payload, 1)[0]
            elif key == 0x04 and len(payload) >= 2:
                self._config['volume'] = payload[1]
            elif key == 0x05 and len(payload) >= 2:
                self._config['agc_target'] = payload[1]
            else:
                return nak(ERR_INVALID_PARAM)

            return respond(RSP_SET_CONFIG, bytes([0]))

        # ---- STREAM_READ ------------------------------------------- #
        if cmd == CMD_STREAM_READ:
            if self._state != State.IDLE:
                return nak(ERR_INVALID_STATE)
            self._state = State.STREAM
            return respond(RSP_STREAM_READ, bytes([0]))

        # ---- STREAM_WRITE ------------------------------------------ #
        if cmd == CMD_STREAM_WRITE:
            if self._state != State.IDLE:
                return nak(ERR_INVALID_STATE)
            self._state = State.STREAM
            return respond(RSP_STREAM_WRITE, bytes([0]))

        # ---- Unknown ----------------------------------------------- #
        return nak(ERR_UNKNOWN_CMD)

    def _nak(self, code: int) -> bytes:
        """Build a NAK response. The command ID is unknown at this point,
        so we use a generic error packet."""
        return encode_packet(0xFF, bytes([code]))

    # ---- Tape Inspection (for tests) ------------------------------- #

    @property
    def tape(self) -> list[RawBlock]:
        """The raw blocks stored on the simulated tape."""
        return list(self._tape)

    @property
    def tape_size(self) -> int:
        return len(self._tape)

    # ---- Error Injection ------------------------------------------- #

    def set_error_crc(self, every_n: int):
        """Inject a CRC error into every Nth read response."""
        self._error_crc_rate = every_n

    def set_error_drop(self, every_n: int):
        """Drop every Nth response (simulate packet loss)."""
        self._error_drop_rate = every_n

    def set_error_no_tape(self, enabled: bool):
        """Simulate no cassette in the deck."""
        self._error_no_tape = enabled

    def set_error_write_protect(self, enabled: bool):
        """Simulate write-protect tab broken."""
        self._error_write_protect = enabled

    # ---- Standalone TCP Server ------------------------------------- #

    def run_tcp(self, host: str = '127.0.0.1', port: int = 9725):
        """
        Start TCP server. Accepts one connection and speaks the
        binary transport protocol.
        
        Each message is a framed packet (0xFE marker, escaped, CRC'd).
        """
        import socket
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        print(f"[DummyMCU] TCP server listening on {host}:{port}")

        conn, addr = server.accept()
        print(f"[DummyMCU] Connected by {addr}")
        self._run_stream(conn)
        conn.close()

    # ---- Stdio Mode ------------------------------------------------ #

    def run_stdio(self):
        """Read framed packets from stdin, write responses to stdout."""
        print("[DummyMCU] Stdio mode — reading from stdin", file=sys.stderr)
        self._run_stream(sys.stdin.buffer, sys.stdout.buffer)

    # ---- Internal Stream Handler ----------------------------------- #

    def _run_stream(self, rfile, wfile=None):
        """
        Bidirectional framed-packet stream.
        
        Reads one command, processes it, writes the response.
        Repeats until the stream closes.
        """
        if wfile is None:
            wfile = rfile  # same socket object

        buf = b''
        while True:
            # Read raw bytes
            try:
                chunk = rfile.read(4096)
            except Exception:
                break
            if not chunk:
                break

            buf += chunk

            # Try to extract a complete packet
            while True:
                idx = buf.find(bytes([START_MARKER]))
                if idx < 0:
                    buf = b''
                    break
                if idx > 0:
                    buf = buf[idx:]  # sync to marker

                # Minimum packet: marker(1) + len(2) + cmd(1) + crc(2) = 6
                if len(buf) < 6:
                    break

                # Parse length from the escaped inner data
                # We need to find the end... approximate:
                # Conservative: packet is at most 6 + 65535 + some escape overhead
                inner = buf[1:1 + 5]  # at least len(2)+cmd(1)
                # We'll just try to decode
                pkt = decode_packet(buf)
                if pkt is None:
                    # Try next byte
                    buf = buf[1:]
                    continue

                # Got a valid packet — process it
                response = self._process_command(buf)
                if response:
                    wfile.write(response)
                    try:
                        wfile.flush()
                    except Exception:
                        pass

                # Remove consumed bytes
                consumed = len(buf)  # rough; in practice, track exact size
                buf = b''
                break


# ======================================================================
#  Quick test
# ======================================================================

if __name__ == "__main__":
    # Quick smoke test: PING
    mcu = DummyMCU()
    ping = encode_packet(CMD_PING)
    mcu.write_callback(RawBlock(bytes=ping))
    rsp = mcu.read_callback()
    if rsp:
        pkt = decode_packet(rsp.bytes)
        if pkt and pkt['cmd_id'] == RSP_PING:
            print(f"[DummyMCU] PING OK — version: {pkt['payload'].rstrip(b'\\x00').decode()}")
        else:
            print(f"[DummyMCU] PING FAIL — response: {pkt}")
    else:
        print("[DummyMCU] No response to PING")
