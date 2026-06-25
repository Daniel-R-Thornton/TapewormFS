"""
tape_channel.py — Simulates the physical cassette tape channel.

Models real-world cassette imperfections:
  • Wow      — slow speed drift (0.5–3 Hz, ±2–5%)
  • Flutter  — fast speed jitter (2–10 Hz, ±0.5–2%)
  • Noise    — tape hiss + 50/60 Hz hum
  • Dropouts — brief amplitude fades
  • Head alignment — L/R phase shift and crosstalk

This lets us test the modem under realistic conditions without
a real cassette deck.
"""

import math
import random
import struct
from dataclasses import dataclass, field
from typing import Callable, Optional


# ======================================================================
#  Tape Channel Configuration
# ======================================================================

@dataclass
class TapeChannelConfig:
    """Tweak these to model different decks / tape conditions."""

    # Speed instability
    wow_depth_hz: float = 0.5        # Wow oscillation frequency (slow)
    wow_depth_pct: float = 1.5       # Peak speed variation (±%)
    flutter_depth_hz: float = 4.0    # Flutter oscillation frequency (fast)
    flutter_depth_pct: float = 0.5   # Peak speed variation (±%)

    # Noise
    noise_db: float = -25.0          # White noise floor (dB rel. signal)
    hum_50hz_db: float = -50.0       # 50 Hz mains hum level
    hum_60hz_db: float = -55.0       # 60 Hz mains hum level (US)

    # Dropouts
    dropout_rate: float = 0.005      # Probability of dropout per second
    dropout_depth_db: float = -15.0  # How deep the dropout is
    dropout_duration_s: float = 0.03 # Typical dropout length

    # Head / channel
    head_phase_shift_deg: float = 2.0  # Phase misalignment between L/R
    crosstalk_db: float = -40.0       # Leakage between L and R channels
    dc_offset: float = 0.005          # Small DC offset from preamp

    # Recording
    saturation_level: float = 0.85    # Max amplitude before clipping
    bias_noise_db: float = -55.0      # Tape bias noise (recording)

    # Sample rate
    sample_rate: int = 44100          # System sample rate (Hz)


# ======================================================================
#  Tape Channel — applies effects to an audio signal
# ======================================================================

class TapeChannel:
    """
    Simulates recording to and playing back from cassette.

    Usage:
        channel = TapeChannel()
        clean_audio = [...]  # float samples in [-1, 1]

        # Record (adds bias noise, saturation)
        recorded = channel.record(clean_audio)

        # Playback (adds wow, flutter, noise, dropouts)
        playback = channel.playback(recorded)
    """

    def __init__(self, config: Optional[TapeChannelConfig] = None):
        self.cfg = config or TapeChannelConfig()
        self._time = 0.0  # seconds since start

        # State for continuous effects
        self._wow_phase = random.uniform(0, 2 * math.pi)
        self._flutter_phase = random.uniform(0, 2 * math.pi)
        self._dropout_timer = 0.0
        self._in_dropout = False
        self._dropout_remaining = 0.0

    def reset(self):
        """Reset the channel state (call between recordings)."""
        self._time = 0.0
        self._wow_phase = random.uniform(0, 2 * math.pi)
        self._flutter_phase = random.uniform(0, 2 * math.pi)
        self._dropout_timer = 0.0
        self._in_dropout = False
        self._dropout_remaining = 0.0

    # ---- Speed variation (wow + flutter) -------------------------- #

    def _speed_ratio(self, dt: float) -> float:
        """
        Return instantaneous speed multiplier at current time.
        1.0 = nominal speed.
        >1.0 = tape running fast (higher pitch)
        <1.0 = tape running slow (lower pitch)
        """
        self._wow_phase += 2 * math.pi * self.cfg.wow_depth_hz * dt
        self._flutter_phase += 2 * math.pi * self.cfg.flutter_depth_hz * dt

        wow = (self.cfg.wow_depth_pct / 100.0) * math.sin(self._wow_phase)
        flutter = (self.cfg.flutter_depth_pct / 100.0) * math.sin(self._flutter_phase)

        return 1.0 + wow + flutter

    # ---- Noise generation ----------------------------------------- #

    def _noise(self, num_samples: int, sr: float) -> list[float]:
        """Generate noise floor samples."""
        white = [random.gauss(0, 1) for _ in range(num_samples)]

        # Scale to desired noise level
        noise_rms = 10 ** (self.cfg.noise_db / 20.0)
        result = [s * noise_rms for s in white]

        # Add 50 Hz hum
        if self.cfg.hum_50hz_db > -100:
            hum50_rms = 10 ** (self.cfg.hum_50hz_db / 20.0)
            for i in range(num_samples):
                t = self._time + i / sr
                result[i] += hum50_rms * math.sin(2 * math.pi * 50 * t)

        # Add 60 Hz hum
        if self.cfg.hum_60hz_db > -100:
            hum60_rms = 10 ** (self.cfg.hum_60hz_db / 20.0)
            for i in range(num_samples):
                t = self._time + i / sr
                result[i] += hum60_rms * math.sin(2 * math.pi * 60 * t)

        return result

    # ---- Dropouts ------------------------------------------------- #

    def _apply_dropouts(self, signal: list[float], sr: float) -> list[float]:
        """Apply random amplitude dropouts."""
        result = list(signal)
        dropout_gain = 10 ** (self.cfg.dropout_depth_db / 20.0)
        dt_per_sample = 1.0 / sr

        for i in range(len(result)):
            self._time += dt_per_sample

            if self._in_dropout:
                result[i] *= dropout_gain
                self._dropout_remaining -= dt_per_sample
                if self._dropout_remaining <= 0:
                    self._in_dropout = False
            else:
                self._dropout_timer += dt_per_sample
                if self._dropout_timer >= 1.0 / max(self.cfg.dropout_rate, 0.001):
                    self._dropout_timer = 0.0
                    if random.random() < self.cfg.dropout_rate:
                        self._in_dropout = True
                        self._dropout_remaining = self.cfg.dropout_duration_s * (
                            0.5 + random.random()
                        )

        return result

    # ---- Saturation / clipping ------------------------------------ #

    def _saturate(self, signal: list[float]) -> list[float]:
        """Soft clip at saturation level (analogue tape compression)."""
        result = []
        sat = self.cfg.saturation_level
        for s in signal:
            if s > sat:
                s = sat + (s - sat) / (1 + (s - sat) / 0.3)
            elif s < -sat:
                s = -sat + (s + sat) / (1 - (s + sat) / 0.3)
            result.append(s)
        return result

    # ---- Head crosstalk ------------------------------------------- #

    def _apply_crosstalk(self, left: list[float], right: list[float]) -> tuple[list[float], list[float]]:
        """Mix a little of each channel into the other."""
        xtalk_gain = 10 ** (self.cfg.crosstalk_db / 20.0) if self.cfg.crosstalk_db > -100 else 0.0
        n = min(len(left), len(right))
        L = list(left)
        R = list(right)
        for i in range(n):
            L[i] += xtalk_gain * right[i]
            R[i] += xtalk_gain * left[i]
        return L, R

    # ---- Phase shift (head misalignment) -------------------------- #

    def _phase_shift(self, signal: list[float], sr: float) -> list[float]:
        """Delay the signal slightly to simulate head azimuth error."""
        delay_samples = int((self.cfg.head_phase_shift_deg / 360.0) * sr / 1000.0)
        if delay_samples <= 0:
            return signal
        return [0.0] * delay_samples + signal[:-delay_samples]

    # ---- Public API ----------------------------------------------- #

    def record(self, signal: list[float], sr: Optional[int] = None) -> list[float]:
        """
        Simulate recording to tape.
        Adds bias noise and applies saturation.
        """
        if sr is None:
            sr = self.cfg.sample_rate

        # Bias noise
        bias = 10 ** (self.cfg.bias_noise_db / 20.0)
        result = [s + random.gauss(0, bias) for s in signal]

        # Saturation
        result = self._saturate(result)

        # DC offset
        result = [s + self.cfg.dc_offset for s in result]

        return result

    def playback(self, signal: list[float], sr: Optional[int] = None,
                 channel: str = 'mono') -> list[float]:
        """
        Simulate playback from tape.
        
        Applies wow, flutter, noise, dropouts.
        If channel='stereo', also applies head phase shift and crosstalk.
        Returns audio as float samples.
        """
        if sr is None:
            sr = self.cfg.sample_rate

        # Wow and flutter through resampling
        # We simulate this by time-varying the output sample rate
        output = []
        read_pos = 0
        total_samples = len(signal)
        self._time = 0.0

        out_idx = 0
        while read_pos < total_samples - 1:
            dt = 1.0 / sr
            speed = self._speed_ratio(dt)
            read_pos += speed

            # Linear interpolation
            idx = int(read_pos)
            frac = read_pos - idx
            if idx + 1 < total_samples:
                s = signal[idx] * (1 - frac) + signal[idx + 1] * frac
            else:
                s = signal[-1]

            output.append(s)
            out_idx += 1

        # Add noise
        noise = self._noise(len(output), sr)
        output = [output[i] + noise[i] for i in range(len(output))]

        # Dropouts
        self._time = 0.0
        self._dropout_timer = 0.0
        self._in_dropout = False
        output = self._apply_dropouts(output, sr)

        return output

    def playback_stereo(self, left: list[float], right: list[float],
                        sr: Optional[int] = None) -> tuple[list[float], list[float]]:
        """
        Stereo playback with crosstalk and phase shift.
        """
        if sr is None:
            sr = self.cfg.sample_rate

        # Phase shift on right channel (head misalignment)
        right_shifted = self._phase_shift(right, sr)

        # Playback each channel
        L = self.playback(left, sr, channel='L')
        R = self.playback(right_shifted, sr, channel='R')

        # Crosstalk
        L, R = self._apply_crosstalk(L, R)

        return L, R


# ======================================================================
#  Quick test
# ======================================================================

if __name__ == "__main__":
    import matplotlib.pyplot as plt

    # Generate a test tone
    sr = 44100
    duration = 2.0
    t = [i / sr for i in range(int(sr * duration))]
    tone = [math.sin(2 * math.pi * 1000 * ti) for ti in t]

    print(f"Test tone: {len(tone)} samples, {duration}s")

    channel = TapeChannel()
    recorded = channel.record(tone)
    playback = channel.playback(recorded)

    print(f"Playback: {len(playback)} samples")
    print(f"  Speed ratio range: {min(abs(playback)):.4f} – {max(abs(playback)):.4f}")

    # Plot first 5000 samples
    plt.figure(figsize=(12, 6))
    plt.subplot(2, 1, 1)
    plt.plot(t[:5000], tone[:5000], label='Original')
    plt.plot(t[:min(5000, len(playback))], playback[:5000], label='Playback', alpha=0.7)
    plt.legend()
    plt.title('Tape Channel: Original vs Playback')
    plt.ylabel('Amplitude')

    plt.subplot(2, 1, 2)
    plt.specgram(playback, NFFT=256, Fs=sr, cmap='viridis')
    plt.title('Spectrogram of playback')
    plt.ylabel('Frequency (Hz)')

    plt.tight_layout()
    plt.savefig('/tmp/tape_channel_test.png')
    print("Saved plot to /tmp/tape_channel_test.png")
