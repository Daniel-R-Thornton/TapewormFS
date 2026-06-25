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
import math
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
    
    Stdio mode (subprocess, serial-like pipe):
        mcu = DummyMCU()
        mcu.run_stdio()
        mcu = DummyMCU()
        mcu.run_stdio()
    """

    def __init__(self, initial_tape: Optional[list[RawBlock]] = None,
                 speed: str = 'fast'):
        """
        Args:
            initial_tape: Pre-populated blocks.
            speed: 'fast' (unit tests) or 'realistic' (hardware-accurate delays).
        """
        # Tape storage — blocks stored as (block_number, RawBlock) tuples
        # to track where each block lives on the physical tape
        self._tape: list[tuple[int, RawBlock]] = []
        if initial_tape:
            for i, b in enumerate(initial_tape):
                self._tape.append((i, b))

        # Physical tape map: block_number -> position_ms from BOT
        # This simulates actual physical locations on the tape
        self._tape_map: dict[int, int] = {}
        self._next_phys_block = 0  # next physical block number

        # State machine
        self._state = State.IDLE
        self._pos = 0  # current position in tape array
        self._write_buffer: list[tuple[int, RawBlock]] = []  # buffered writes

        # Simulated flags
        self._tape_present = True
        self._write_protect = False
        self._bot = True
        self._eot = False
        self._pos_locked = True

        # Config (from SET_CONFIG)
        self._config = {
            'baud_rate': 200,
            'modulation': 0,
            'frame_size': 1024,
            'volume': 128,
            'agc_target': 128,
        }

        # ---- Realistic timing ---------------------------------- #
        # All delays are based on actual tape physics:
        #   - Read/write time = bits_per_block / baud_rate
        #   - Seek time = |pos_a - pos_b| / tape_speed_mm_s
        #
        # A C60 cassette runs at ~4.76 cm/s. Blocks are ~1 cm apart = ~210ms/block.

        self._baud_rate = 200
        self._frame_size = 1024       # bytes per frame on tape
        self._tape_speed_mm_s = 47.6  # standard cassette: 4.76 cm/s
        self._block_spacing_mm = 10.0 # approximate mm between blocks on tape
        self._motor_start_ms = 300    # time to spin motor up to speed

        if speed == 'realistic':
            # Hardware-accurate delays (slow — for demos)
            self._mode = 'realistic'
        else:
            # Fast mode (for unit tests) — delays are scaled down
            # but still proportional to distance
            self._mode = 'fast'

        # ---- Wow/flutter simulation ----------------------------- #
        self._wow_period_ms = 2000
        self._wow_depth = 0.15
        self._flutter_period_ms = 250
        self._flutter_depth = 0.05
        self._sim_time = 0.0

        # ---- Error injection ------------------------------------ #
        self._error_crc_rate = 0
        self._error_drop_rate = 0
        self._error_no_tape = False
        self._error_write_protect = False
        self._op_count = 0

        # ---- Transient error simulation ------------------------- #
        self._max_retries = 3
        self._dropout_prob = 0.01
        self._crc_error_prob = 0.02

        # For transport callbacks
        self._incoming: Optional[RawBlock] = None
        self._outgoing: Optional[RawBlock] = None

    # ---- Raw block callbacks (for Filesystem) -------------------- #
    # These bypass the packet protocol — they store/retrieve blocks
    # directly, as if talking to a raw tape device.

    def _block_time_ms(self) -> float:
        """
        Time to transfer one block at the current baud rate.
        
        A block of frame_size bytes takes:
          frame_size * 8 bits/byte / baud_rate seconds
        Plus modem overhead (sync preamble, guard intervals).
        """
        raw_bits = self._frame_size * 8
        overhead_factor = 1.3  # 30% overhead for sync/guards
        return (raw_bits / self._baud_rate) * 1000 * overhead_factor

    def _block_spacing_ms(self) -> float:
        """
        Time for the tape to travel from one block to the next.
        
        At standard cassette speed (4.76 cm/s), blocks spaced
        ~10 mm apart take 10 / 47.6 * 1000 = ~210 ms.
        """
        return (self._block_spacing_mm / self._tape_speed_mm_s) * 1000

    def _travel_time_ms(self, from_block: int, to_block: int) -> float:
        """
        Time to move the tape from one block position to another.
        Includes motor start-up time.
        """
        if from_block == to_block:
            return 0
        distance = abs(to_block - from_block)
        travel = distance * self._block_spacing_ms()
        return self._motor_start_ms + travel

    def _block_phys_pos(self, block_no: int) -> int:
        """Get the physical tape position (ms from BOT) of a block."""
        # Estimate: blocks are evenly spaced on tape
        return int(block_no * self._block_spacing_ms())

    # ---- Scaling ------------------------------------------------ #
    # In 'fast' mode, delays are divided by this factor so unit
    # tests don't take forever.  The *proportions* are preserved.

    @property
    def _time_scale(self) -> float:
        return 0.001 if self._mode == 'fast' else 1.0

    # ---- Timing simulation (wow/flutter) ------------------------- #

    def _wow_flutter_delay(self, base_ms: float) -> float:
        """
        Apply wow and flutter to a base delay.
        
        Returns the actual delay in milliseconds, which varies
        sinusoidally to simulate tape speed instability.
        """
        wow = self._wow_depth * math.sin(2 * math.pi * self._sim_time /
                                          (self._wow_period_ms / 1000.0))
        flutter = self._flutter_depth * math.sin(2 * math.pi * self._sim_time /
                                                  (self._flutter_period_ms / 1000.0) +
                                                  math.pi / 3)
        variation = 1.0 + wow + flutter
        scaled = base_ms * self._time_scale * variation
        return max(scaled, 0.1)  # at least 0.1ms

    def _advance_time(self, dt_ms: float):
        self._sim_time += dt_ms / 1000.0

    # ---- Transient error simulation ------------------------------ #

    def _maybe_inject_error(self) -> tuple[bool, bool]:
        """
        Simulate transient tape errors.
        
        Returns (has_crc_error: bool, is_dropout: bool).
        Errors are transient — retry usually succeeds.
        """
        has_crc = random.random() < self._crc_error_prob
        is_drop = random.random() < self._dropout_prob
        return has_crc, is_drop

    def _simulate_read_delay(self, block_no: int = 0, is_retry: bool = False):
        """Sleep for a realistic read time."""
        delay = self._block_time_ms()
        if is_retry:
            delay *= 1.5  # retries take longer (re-syncing)
        delay = self._wow_flutter_delay(delay)
        time.sleep(delay / 1000.0)
        self._advance_time(delay)

    def _simulate_write_delay(self):
        """Sleep for a realistic write time."""
        delay = self._wow_flutter_delay(self._block_time_ms())
        time.sleep(delay / 1000.0)
        self._advance_time(delay)

    def _simulate_seek_delay(self, from_block: int, to_block: int):
        """Sleep for seek time proportional to physical distance."""
        if from_block == to_block:
            return
        delay = self._travel_time_ms(from_block, to_block)
        delay = self._wow_flutter_delay(delay)
        time.sleep(delay / 1000.0)
        self._advance_time(delay)

    def raw_write(self, block: RawBlock) -> bool:
        """Store a raw block on tape."""
        if self._error_no_tape or self._error_write_protect:
            return False
        self._simulate_write_delay()

        # Store block and record its physical position
        block_no = self._next_phys_block
        self._next_phys_block += 1

        if self._pos >= len(self._tape):
            self._tape.append((block_no, block))
        else:
            self._tape[self._pos] = (block_no, block)

        self._tape_map[block_no] = self._block_phys_pos(block_no)
        self._pos += 1
        return True

    def raw_read(self) -> Optional[RawBlock]:
        """Read the next raw block from tape.

        May simulate transient errors (CRC noise, dropout).
        Errors are retried silently up to _max_retries times.
        """
        if self._pos >= len(self._tape):
            return None

        for attempt in range(self._max_retries):
            block_no, rb = self._tape[self._pos]
            self._simulate_read_delay(block_no, attempt > 0)
            crc_err, dropout = self._maybe_inject_error()

            if dropout:
                continue

            if crc_err:
                buf = bytearray(rb.bytes)
                if len(buf) > 4:
                    buf[-4] ^= 0xFF
                    rb = RawBlock(bytes=bytes(buf))
                self._pos += 1
                return rb

            self._pos += 1
            return rb

        return None

    def raw_seek(self, block_no: int) -> bool:
        """Position the tape head at a given block.

        Seek time depends on physical distance between current
        and target position on the tape (realistic).
        """
        if self._error_no_tape:
            return False

        # Find current physical position from tape map
        current_phys = 0
        if 0 <= self._pos < len(self._tape):
            current_phys = self._tape_map.get(self._tape[self._pos][0], 0)

        # Target physical position
        target_phys = self._block_phys_pos(block_no)

        # Seek based on physical distance, not block count
        self._simulate_seek_delay(current_phys, target_phys)

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

            # Simulate write delay (realistic timing at current baud rate)
            if self._mode == 'realistic' and len(self._write_buffer) <= 3:
                delay = self._wow_flutter_delay(self._block_time_ms())
                time.sleep(delay / 1000)

            return respond(RSP_WRITE_BLOCK, bytes([0]))

        # ---- READ_NEXT --------------------------------------------- #
        if cmd == CMD_READ_NEXT:
            if self._state not in (State.IDLE, State.READ):
                return nak(ERR_INVALID_STATE)

            self._state = State.READ

            # Check if we're past end of tape
            if self._pos >= len(self._tape):
                return nak(ERR_NO_TAPE)  # "no more blocks"

            # Simulate read delay (realistic timing at current baud rate)
            delay = self._wow_flutter_delay(self._block_time_ms())
            time.sleep(delay / 1000)

            block_raw = self._tape[self._pos]
            if isinstance(block_raw, tuple):
                _, block_rb = block_raw
            else:
                block_rb = block_raw
            block = block_rb
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
            delay = self._wow_flutter_delay(self._block_time_ms())
            time.sleep(delay / 1000)

            for rb in self._write_buffer:
                bn = self._next_phys_block
                self._next_phys_block += 1
                self._tape.append((bn, rb))
                self._tape_map[bn] = self._block_phys_pos(bn)
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
        return [rb for (_, rb) in self._tape]

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
