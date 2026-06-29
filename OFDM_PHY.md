# TapewormFS — Physical Layer & Architecture (Revised)

## Phase 1: "Working Prototype on Real Cassette"

**Goal:** Plug‑in USB drive, instantly usable, tapes store ~90‑360 KB per side, background sync.

---

## Physical Layer: Hybrid FSK + Pilot Tone

**Primary modulation:** Multi‑tone FSK (original FrequencyPulse)
- 4–16 tones, 100–200 symbols/s → 200–1600 bps raw
- Simple to decode: Goertzel filter bank or zero‑crossing discriminator
- Proven in `debug-suite/`, easy to port to ESP32 C

**Continuous pilot tone:** 62.5 Hz sine/square, placed below the FSK band
- Transmitted together with FSK symbols (mixed into the audio)
- Provides instantaneous tape‑speed measurement (by measuring pilot period) to adapt symbol timing — handles ±3% wow/flutter
- Enables fast‑wind block counting: during fast‑forward, pilot frequency shifts up; count zero‑crossings to estimate blocks passed

**Block boundary markers:** every block (or every N FSK symbols), invert the pilot phase or insert a short gap. The receiver detects these glitches even in fast‑wind to find block starts.

---

## Tape Layout & Mirroring

- Side A split into **Forward** (beginning → middle) and **Reverse** (end → middle), both containing identical data
- **Directory superblock** resides in the middle
- When reading a file block, firmware picks the copy nearest the current head position
- Seek time halved on average (worst‑case 15 min on a C60)
- Stereo L/R channels both carry the same FSK+pilot signal for redundancy

---

## Filesystem & Protocol (unchanged from current)

- ESP32‑S3 runs USB MSC (TinyUSB) exposing FAT32 on SD card
- Background sync task writes dirty files to tape using the FSK+pilot modem
- Block structure: 32 bytes user data + CRC‑32, wrapped in RS(255,239) FEC across blocks
- Protocol between MCU and host (via USB CDC): `[0xFE | len | cmd_id | payload | crc16]` with byte escaping

---

## Implementation Priority (Phase 1)

1. Port the existing FSK modem to ESP32 C (using I2S for DAC/ADC)
2. Add the 62.5 Hz pilot generation and detection
3. Implement pilot‑based adaptive symbol clock — measure pilot zero‑crossings, adjust symbol timer
4. Add block boundary markers (pilot phase flip) and a fast‑wind counter
5. Build mirroring logic in the filesystem layer
6. Integrate USB MSC + SD cache with background sync

---

## Expected Performance (Phase 1)

| Metric | Value |
|--------|-------|
| Raw bit rate (FSK) | 200–1600 bps (configurable) |
| Net throughput | ~100–800 B/s (after FEC + framing) |
| C60 capacity per side | 90–360 KB |
| Pilot‑aided seek accuracy | Within 1–2 blocks |
| Worst‑case seek time | ~15 min (with mirroring) |

---

## Phase 2: OFDM Turbo Mode (future upgrade)

Once the basic system works end‑to‑end on real tape, add OFDM as an optional high‑density mode:

- 64‑point FFT, 8–16 active subcarriers, QPSK/16‑QAM
- Reuse the same 62.5 Hz pilot for clock recovery
- Auto‑negotiate per block: if the channel is clean, switch to OFDM turbo; else fallback to FSK

---

## Code Snippets for Phase 1

### Pilot Tone Generation (mixing with FSK)

```c
// Generate one audio sample with FSK + pilot
float generate_sample(float fsk_tone_freq, float pilot_freq, float sample_rate,
                      float *fsk_phase, float *pilot_phase) {
    float sample = 0.0f;

    // FSK tone (simple sine)
    sample += sinf(2.0f * M_PI * (*fsk_phase));
    *fsk_phase += fsk_tone_freq / sample_rate;
    if (*fsk_phase >= 1.0f) *fsk_phase -= 1.0f;

    // Pilot tone (62.5 Hz square wave, 0.1 amplitude)
    float pilot = (*pilot_phase < 0.5f) ? 0.1f : -0.1f;
    *pilot_phase += pilot_freq / sample_rate;
    if (*pilot_phase >= 1.0f) *pilot_phase -= 1.0f;

    return sample + pilot;
}
```

### Pilot Period Measurement for Adaptive Symbol Clock

```c
// Measure pilot zero-crossing period in samples
// On each positive zero-cross, compute instantaneous speed ratio
volatile float speed_ratio = 1.0f;  // 1.0 = nominal

void ISR_ADC_new_sample() {
    static float last_sample = 0;
    float s = read_adc();
    if (last_sample < 0 && s >= 0) { // positive zero-cross
        static uint32_t last_time;
        uint32_t now = get_micros();
        float period = now - last_time;
        float nominal = 1e6 / 62.5; // 16000 µs
        speed_ratio = nominal / period; // >1 if tape fast, <1 if slow
        last_time = now;
        // Update symbol timer prescaler using speed_ratio
    }
    last_sample = s;
}
```

### Block Boundary Detection via Pilot Phase Flip

```c
// Detect a phase reversal in the pilot tone
// If pilot phase suddenly flips by 180°, a block boundary just occurred.
void detect_block_boundary(float pilot_filtered) {
    static float prev_pilot = 0;
    static bool block_was_flipped = false;
    // Check for sign change with low amplitude -> likely a flip
    if (fabsf(pilot_filtered) < 0.05 && (prev_pilot * pilot_filtered < 0)) {
        if (!block_was_flipped) {
            block_boundary_callback();
            block_was_flipped = true;
        }
    } else {
        block_was_flipped = false;
    }
    prev_pilot = pilot_filtered;
}
```

### Mirroring Seek Logic

```python
def pick_closest_copy(current_block, target_block, total_blocks):
    """Returns direction ('fwd'/'rev') and block offset to seek."""
    mid = total_blocks // 2
    fwd_dist = (target_block - current_block) % total_blocks
    rev_copy = total_blocks - 1 - target_block
    rev_dist = (current_block - rev_copy) % total_blocks
    if fwd_dist <= rev_dist:
        return 'fwd', fwd_dist
    else:
        return 'rev', rev_dist
```

---

## What's Already Built (Reusable in Phase 1)

- `filesystem/tapefs.py` — filesystem logic, CRC, RS, Directory, tests
- `filesystem/serial_transport.py` — packet protocol
- `debug-suite/` — FSK generator/visualizer (TypeScript), ready to add pilot tone visualization
- `filesystem/dummy_mcu.py` — simulated ESP32 for testing
