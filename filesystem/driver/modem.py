"""
modem.py — FSK + Pilot Tone Modem for Cassette Tape.

This is the physical layer encoder/decoder that the ESP32 firmware
would run.  Written in Python for simulation and testing, with the
intent to port to C later.

Modulation scheme (Phase 1):
  • Multi-tone FSK — one sine frequency per symbol value
  • Continuous 62.5 Hz pilot tone mixed into the output
  • Block boundary markers via pilot phase inversion
    
Frame structure:
  [sync_preamble | data_symbols | block_boundary_marker]

Decoder:
  • Goertzel filter bank for tone detection
  • Pilot zero-crossing PLL for clock recovery
  • Phase-flip detection for block boundaries
"""

import math
import random
import struct
from dataclasses import dataclass, field
from typing import Optional


# ======================================================================
#  Modem Configuration
# ======================================================================

@dataclass
class ModemConfig:
    """Modem parameters.  Tweak these for speed vs reliability."""

    # Sample rate
    sample_rate: int = 3200  # Low sample rate — fits in ESP32 memory

    # FSK tones (frequencies in Hz)
    # Using 8 tones = 3 bits per symbol
    fsk_tones_hz: list = field(default_factory=lambda: [
        # All below Nyquist (1600 Hz), no aliasing pairs
        400, 600, 800, 1000,
        1150, 1300, 1450, 1550,
    ])

    # Pilot
    pilot_freq_hz: float = 62.5
    pilot_amplitude: float = 0.15  # Relative to full scale

    # Timing
    symbols_per_second: int = 50  # 20 ms per symbol
    guard_samples: int = 6        # ~2 ms guard at 3200 Hz

    # Frame structure
    sync_symbols: int = 4         # Preamble length
    crc_bits: int = 16            # CRC per frame

    # Decoder
    goertzel_window: int = 32     # Samples per Goertzel integration

    # Block boundary
    boundary_skip_symbols: int = 20  # Flip pilot every N symbols

    @property
    def samples_per_symbol(self) -> int:
        return self.sample_rate // self.symbols_per_second

    @property
    def bits_per_symbol(self) -> int:
        return int(math.log2(len(self.fsk_tones_hz)))

    @property
    def symbol_duration_s(self) -> float:
        return 1.0 / self.symbols_per_second


# ======================================================================
#  Goertzel filter — single-tone DFT
# ======================================================================

class ToneDetector:
    """
    Detects the strongest FSK tone in a buffer using correlation
    (sine/cosine dot product).  Simpler and more robust than Goertzel.
    """

    @staticmethod
    def detect(samples: list[float], tone_freqs: list[float],
               sample_rate: float) -> int:
        """
        Return index of the strongest tone in *samples*.
        Uses correlation against sine and cosine references.
        """
        n = len(samples)
        best_idx = 0
        best_mag = -1.0

        for idx, freq in enumerate(tone_freqs):
            # Correlate with sine and cosine
            sin_corr = 0.0
            cos_corr = 0.0
            for i, s in enumerate(samples):
                phase = 2.0 * math.pi * freq * i / sample_rate
                sin_corr += s * math.sin(phase)
                cos_corr += s * math.cos(phase)

            mag = math.sqrt(sin_corr * sin_corr + cos_corr * cos_corr) / n

            if mag > best_mag:
                best_mag = mag
                best_idx = idx

        return best_idx


# ======================================================================
#  Modem Encoder
# ======================================================================

class ModemEncoder:
    """
    Converts bytes into audio samples (FSK + pilot).

    Usage:
        encoder = ModemEncoder()
        samples = encoder.encode(b"Hello tape!")
    """

    def __init__(self, config: Optional[ModemConfig] = None):
        self.cfg = config or ModemConfig()
        self._pilot_phase = 0.0
        self._symbol_count = 0

    def reset(self):
        self._pilot_phase = 0.0
        self._symbol_count = 0

    def _pilot_sample(self) -> float:
        """Generate one sample of the pilot tone."""
        val = math.sin(2 * math.pi * self._pilot_phase)
        self._pilot_phase += self.cfg.pilot_freq_hz / self.cfg.sample_rate
        if self._pilot_phase >= 1.0:
            self._pilot_phase -= 1.0
        return val * self.cfg.pilot_amplitude

    def _pilot_sample_with_flip(self, flip: bool = False) -> float:
        """Pilot sample, optionally phase-inverted for block boundary."""
        val = math.sin(2 * math.pi * self._pilot_phase + (math.pi if flip else 0))
        self._pilot_phase += self.cfg.pilot_freq_hz / self.cfg.sample_rate
        if self._pilot_phase >= 1.0:
            self._pilot_phase -= 1.0
        return val * self.cfg.pilot_amplitude

    def _fsk_symbol(self, value: int) -> list[float]:
        """Generate audio samples for one FSK symbol (value = tone index)."""
        freq = self.cfg.fsk_tones_hz[value % len(self.cfg.fsk_tones_hz)]
        samples = []
        sps = self.cfg.samples_per_symbol

        for i in range(sps):
            t = i / self.cfg.sample_rate
            sample = math.sin(2 * math.pi * freq * t)

            # Add pilot
            flip = (self._symbol_count > 0
                    and self._symbol_count % self.cfg.boundary_skip_symbols == 0
                    and i == 0)
            sample += self._pilot_sample_with_flip(flip)

            samples.append(sample)

        self._symbol_count += 1
        return samples

    def _guard(self) -> list[float]:
        """Generate guard interval (silence + pilot)."""
        return [self._pilot_sample() for _ in range(self.cfg.guard_samples)]

    def _sync_symbol(self) -> list[float]:
        """A known symbol used for frame synchronisation."""
        return self._fsk_symbol(0)  # tone 0 = sync marker

    def encode(self, data: bytes) -> list[float]:
        """
        Encode bytes into audio samples.

        Returns float samples in [-1, 1], or empty list if data is empty.
        """
        if not data:
            return []
        output = []

        # Sync preamble (with guards, same as data symbols)
        for _ in range(self.cfg.sync_symbols):
            output.extend(self._sync_symbol())
            output.extend(self._guard())

        # Data: convert to tone indices and modulate
        bits_per_sym = self.cfg.bits_per_symbol
        bitstream = []
        for byte in data:
            for bit_idx in range(8):
                bitstream.append((byte >> (7 - bit_idx)) & 1)

        # Pad to multiple of bits_per_symbol
        while len(bitstream) % bits_per_sym != 0:
            bitstream.append(0)

        for i in range(0, len(bitstream), bits_per_sym):
            value = 0
            for j in range(bits_per_sym):
                if i + j < len(bitstream):
                    value = (value << 1) | bitstream[i + j]

            output.extend(self._fsk_symbol(value))
            output.extend(self._guard())

        # Final guard
        output.extend(self._guard())

        # Normalise
        peak = max(abs(s) for s in output) or 1.0
        output = [s / peak for s in output]

        return output

    def bits_from_bytes(self, data: bytes) -> list[int]:
        """Extract bits from bytes, MSB first."""
        bits = []
        for byte in data:
            for i in range(8):
                bits.append((byte >> (7 - i)) & 1)
        return bits


# ======================================================================
#  Modem Decoder
# ======================================================================

class ModemDecoder:
    """
    Converts audio samples back into bytes (FSK + pilot).

    Buffers signal into symbol windows, runs Goertzel on each to
    determine the dominant tone.

    Usage:
        decoder = ModemDecoder()
        data = decoder.decode(samples)
    """

    def __init__(self, config: Optional[ModemConfig] = None):
        self.cfg = config or ModemConfig()
        self.reset()

    def reset(self):
        pass

    def _strongest_tone(self, samples: list[float]) -> int:
        return ToneDetector.detect(samples, self.cfg.fsk_tones_hz,
                                   self.cfg.sample_rate)

    def _slice_symbols(self, samples: list[float]) -> list[int]:
        """Slice audio into symbol windows, detect tone per window."""
        # Simple bandpass to reduce noise below 300 Hz and above 2 kHz
        # Moving average subtract (crude high-pass)
        filtered = list(samples)
        window = 8
        for i in range(len(filtered)):
            start = max(0, i - window)
            end = min(len(filtered), i + window + 1)
            avg = sum(filtered[start:end]) / (end - start)
            filtered[i] = samples[i] - avg * 0.95

        sps = self.cfg.samples_per_symbol     # samples per symbol
        guard = self.cfg.guard_samples          # guard between symbols
        step = sps + guard
        tones: list[int] = []

        pos = 0
        while pos + sps <= len(filtered):
            window_samples = filtered[pos:pos + sps]
            tone = self._strongest_tone(window_samples)
            tones.append(tone)
            pos += step

        return tones

    def decode(self, samples: list[float]) -> Optional[bytes]:
        """Decode audio samples into bytes."""
        if len(samples) < self.cfg.samples_per_symbol * (self.cfg.sync_symbols + 1):
            return None
        self.reset()

        tones = self._slice_symbols(samples)
        if len(tones) < self.cfg.sync_symbols:
            return None

        # Find sync preamble (consecutive tone-0 symbols)
        sync_end = -1
        for i in range(len(tones) - self.cfg.sync_symbols + 1):
            if all(t == 0 for t in tones[i:i + self.cfg.sync_symbols]):
                sync_end = i + self.cfg.sync_symbols
                break

        if sync_end < 0:
            return None

        # Data tones follow sync
        data_tones = tones[sync_end:]
        bps = self.cfg.bits_per_symbol

        # Convert tones → bits → bytes
        output = bytearray()
        bits: list[int] = []
        for t in data_tones:
            for j in range(bps - 1, -1, -1):
                bits.append((t >> j) & 1)

        # Convert to bytes, stopping at padding
        for i in range(0, len(bits), 8):
            if i + 8 > len(bits):
                break
            byte = 0
            for j in range(8):
                byte = (byte << 1) | bits[i + j]
            output.append(byte)

        return bytes(output) if output else None


# ======================================================================
#  Full Encoder/Decoder Pipeline Test
# ======================================================================

def encode_decode_roundtrip(data: bytes, config: Optional[ModemConfig] = None,
                            channel_fn=None) -> Optional[bytes]:
    """
    Full pipeline: encode → (optional channel effects) → decode.
    Returns decoded bytes, or None if decoding failed.
    """
    encoder = ModemEncoder(config)
    decoder = ModemDecoder(config)

    # Encode
    audio = encoder.encode(data)

    # Apply channel effects (if provided)
    if channel_fn:
        audio = channel_fn(audio)

    # Decode
    result = decoder.decode(audio)
    return result


if __name__ == "__main__":
    # Quick test
    cfg = ModemConfig()
    test_data = b"Hello TapewormFS! OFDM is cool."

    print(f"Modem config: {cfg.bits_per_symbol} bits/symbol, "
          f"{cfg.symbols_per_second} symbols/s, "
          f"{cfg.sample_rate} Hz sample rate")

    encoder = ModemEncoder(cfg)
    audio = encoder.encode(test_data)
    print(f"Encoded {len(test_data)} bytes → {len(audio)} audio samples "
          f"({len(audio) / cfg.sample_rate:.1f}s)")

    decoder = ModemDecoder(cfg)
    result = decoder.decode(audio)
    if result:
        print(f"Decoded: {result!r}")
        print(f"Match: {result == test_data}")
    else:
        print("Decode failed — no sync found")
