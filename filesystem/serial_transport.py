"""
SerialTransport — talks to the real MCU over UART/USB CDC.

Connects tapefs.Filesystem to a physical ESP32 or RP2040 over a
serial port, using the framed packet protocol from SPEC.md §3.

Usage:
    from serial_transport import SerialTransport
    from tapefs import Filesystem

    transport = SerialTransport(port="/dev/ttyUSB0", baud=115200)
    fs = Filesystem(transport.write, transport.read, transport.seek)

    fs.format()
    fs.write_file("notes.txt", b"Hello cassette!")
"""

import struct
import time
import threading
from typing import Optional

from tapefs import RawBlock


# ======================================================================
#  Protocol Constants (mirrors dummy_mcu.py)
# ======================================================================

START_MARKER = 0xFE

# Host → MCU commands
CMD_WRITE_BLOCK = 0x05
CMD_READ_NEXT   = 0x06
CMD_FLUSH       = 0x07
CMD_STOP        = 0x08
CMD_SEEK        = 0x03
CMD_REWIND      = 0x04
CMD_PING        = 0x01
CMD_GET_STATUS  = 0x02

# Response IDs = command_id | 0x80
RSP_WRITE_BLOCK = 0x85
RSP_READ_NEXT   = 0x86
RSP_FLUSH       = 0x87
RSP_STOP        = 0x88
RSP_SEEK        = 0x83
RSP_REWIND      = 0x84
RSP_PING        = 0x81
RSP_GET_STATUS  = 0x82

# Timeouts (seconds)
ACK_TIMEOUT     = 5.0    # wait for ACK after sending command
DATA_TIMEOUT    = 120.0  # wait for block data (read)


# ======================================================================
#  Packet encode/decode (same logic as dummy_mcu)
# ======================================================================

def _crc16(data: bytes) -> int:
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
    out = bytearray()
    for b in data:
        if b == 0xFE:
            out.extend([0xFD, 0x01])
        elif b == 0xFD:
            out.extend([0xFD, 0x02])
        else:
            out.append(b)
    return bytes(out)

def _unescape(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFD and i + 1 < len(data):
            if data[i + 1] == 0x01:
                out.append(0xFE)
            elif data[i + 1] == 0x02:
                out.append(0xFD)
            i += 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)

def encode_packet(cmd_id: int, payload: bytes = b'') -> bytes:
    """Build a framed packet: [0xFE | len(2) | cmd(1) | payload | crc16(2)]"""
    inner = struct.pack('<H', 1 + len(payload) + 2)  # cmd + payload + crc
    inner += bytes([cmd_id]) + payload
    inner += struct.pack('<H', _crc16(inner))
    return bytes([START_MARKER]) + _escape(inner)

def decode_packet(data: bytes) -> Optional[dict]:
    """Parse a framed packet. Returns {'cmd_id', 'payload'} or None."""
    if not data or data[0] != START_MARKER:
        return None
    inner = _unescape(data[1:])
    if len(inner) < 5:
        return None
    length = struct.unpack_from('<H', inner, 0)[0]
    if len(inner) < 2 + length:
        return None
    cmd_id = inner[2]
    payload = inner[3:3 + length - 3]  # subtract cmd(1) + crc(2)
    stored_crc = struct.unpack_from('<H', inner, 3 + length - 2)[0]
    if _crc16(inner[:2 + length - 2]) != stored_crc:
        return None
    return {'cmd_id': cmd_id, 'payload': payload}


# ======================================================================
#  SerialTransport
# ======================================================================

class SerialTransport:
    """
    Wraps a pyserial port into Filesystem callbacks.
    
    Each callback sends a framed command packet and waits for the
    matching response.
    
    Callbacks:
        transport.write(raw_block)  → sends WRITE_BLOCK command
        transport.read()            → sends READ_NEXT, returns block
        transport.seek(block_no)    → sends SEEK command
    
    Usage:
        transport = SerialTransport("/dev/ttyUSB0", 115200)
        fs = Filesystem(transport.write, transport.read, transport.seek)
    """

    def __init__(self, port: str, baud: int = 115200, timeout: float = 5.0):
        import serial
        self._ser = serial.Serial(
            port=port,
            baudrate=baud,
            timeout=timeout,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
        )
        self._lock = threading.Lock()
        self._timeout = timeout
        self._last_cmd = 0

    def close(self):
        """Close the serial port."""
        self._ser.close()

    # ---- Low-level send/recv --------------------------------------- #

    def _send_cmd(self, cmd_id: int, payload: bytes = b'') -> None:
        """Send a framed command packet."""
        packet = encode_packet(cmd_id, payload)
        self._ser.write(packet)

    def _recv_response(self, expected_id: int, timeout: float = None) -> Optional[bytes]:
        """
        Read bytes until we get a valid response, or timeout.
        Returns the response payload, or None on timeout/error.
        """
        deadline = time.monotonic() + (timeout or self._timeout)
        buf = b''

        while time.monotonic() < deadline:
            # Read one byte
            byte = self._ser.read(1)
            if not byte:
                continue  # timeout on this read, check overall deadline

            buf += byte

            # Look for a complete packet
            idx = buf.find(bytes([START_MARKER]))
            if idx < 0:
                if len(buf) > 4096:
                    buf = b''  # too much junk, reset
                continue

            buf = buf[idx:]  # align to marker

            # Minimum size: marker(1) + len(2) + cmd(1) + crc(2) = 6
            if len(buf) < 6:
                continue

            pkt = decode_packet(buf)
            if pkt is None:
                # Bad packet — skip one byte and retry
                buf = buf[1:]
                continue

            # Got a valid packet
            if pkt['cmd_id'] == expected_id:
                return pkt['payload']
            elif pkt['cmd_id'] == (expected_id & 0x7F) | 0x80:
                # It's a NAK for this command — payload is error code
                return pkt['payload']
            else:
                # Unexpected response — might be from a previous command
                buf = buf[len(buf):]  # discard everything
                continue

        return None  # overall timeout

    # ---- Filesystem callbacks -------------------------------------- #

    def write(self, block: RawBlock) -> bool:
        """
        Send a block to the MCU.
        
        This sends a WRITE_BLOCK command with the raw block data,
        then waits for ACK.
        """
        with self._lock:
            self._send_cmd(CMD_WRITE_BLOCK, block.bytes)
            response = self._recv_response(RSP_WRITE_BLOCK)

        if response is None:
            return False
        # ACK is a single 0x00 byte; NAK is an error code
        return len(response) == 1 and response[0] == 0x00

    def read(self) -> Optional[RawBlock]:
        """
        Read the next block from tape.
        
        Sends READ_NEXT, waits for the MCU to decode and return a block.
        This can take several seconds (the MCU has to find the next
        sync preamble on the audio stream).
        """
        with self._lock:
            self._send_cmd(CMD_READ_NEXT)
            response = self._recv_response(RSP_READ_NEXT, timeout=DATA_TIMEOUT)

        if response is None:
            return None
        if len(response) < 1:
            return None

        # Response: block_type(1) + block_data(N) + crc32(4)
        # If NAK, the payload is a single error code byte
        if len(response) == 1:
            return None  # NAK (end of tape or error)

        # Strip the block type byte and trailing CRC-32
        block_data = response[1:-4]
        return RawBlock(bytes=block_data)

    def seek(self, block_no: int) -> bool:
        """
        Seek to a block number on tape.
        
        Sends SEEK command with the target position.
        """
        pos_payload = struct.pack('<II', block_no, 0)  # block_no, byte_offset
        pos_payload += struct.pack('<I', block_no * 1000)  # tape_ms
        pos_payload += bytes([0, 255])  # side=A, confidence=255

        with self._lock:
            self._send_cmd(CMD_SEEK, pos_payload)
            response = self._recv_response(RSP_SEEK)

        return response is not None and len(response) == 1 and response[0] == 0x00


# ======================================================================
#  Quick sanity check
# ======================================================================

if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage: python serial_transport.py <port> [baud]")
        print("  Tests connection to the MCU (PING).")
        sys.exit(1)

    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    # Quick PING test
    transport = SerialTransport(port, baud)
    try:
        # Send PING directly and check response
        transport._send_cmd(CMD_PING)
        rsp = transport._recv_response(RSP_PING, timeout=2.0)

        if rsp:
            version = rsp.rstrip(b'\x00').decode()
            print(f"MCU alive!  Version: {version}")
        else:
            print("No response from MCU — check connection and baud rate.")
    finally:
        transport.close()
