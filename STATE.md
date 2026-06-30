# TapewormFS — Project State

## Concept
Store digital files on audio cassette. A host driver mounts a folder and syncs files to a simulated ESP32 over UART, which encodes data as audio tones.

## Architecture

```
┌─────────────────────────┐   UART (framed packets)   ┌──────────────────┐
│  Host Computer          │◀─────────────────────────▶│  C++ Firmware    │
│                         │  [0xFE|len|cmd|payload|   │  (ESP32 sim)     │
│  tapeworm (CLI)         │         crc16]            │                  │
│  └ format, ls, read,    │                           │  Modem encoder   │
│    write, mount         │                           │  → MCP4725 (I2S) │
│                         │                           │  ← Onboard ADC   │
│  FUSE mount (write-back │                           │                   │
│    cache to tape)       │                           │  Tape file:      │
│                         │                           │  /tmp/*.wav      │
│  /tmp/tapewormfs_*.wav  │                           └──────────────────┘
└─────────────────────────┘
```

## What Works

### Filesystem (tapefs.py)
- CRC-32, RS(255,239) ECC, Directory, Block serialisation
- `format`, `list_files`, `write_file`, `read_file`
- All unit tests pass (6/6)

### Packet Protocol
- Framed: `[0xFE | len(2) | cmd_id(1) | payload(N) | crc16(2)]`
- Byte escaping (0xFE→FD 01, FD→FD 02)
- Commands: PING, GET_STATUS, SEEK, REWIND, WRITE_BLOCK, READ_NEXT, FLUSH, STOP
- Verified working: PING, SEEK, WRITE_BLOCK, FLUSH, READ_NEXT (returns blocks)

### Firmware Simulation (C++, `cpp/firmware/`)
- ESP32 HAL with file backend (ADC reads from file, DAC writes to file)
- Modem encoder: multi-tone OFDM-style (4 frequencies, 4 bits/frame, 25 fps)
- Motor model (3-wire: FWD/REV/FAST/OFF)
- TapeDeck state machine (idle/seek/read/write)
- TapeMedium — raw audio storage on disk
- Firmware main loop with UART command handler
- Firmware-side debug logging to `/tmp/tapewormfs_fw.log`

### FUSE Mount (`tapeworm mount ./tape_mount`)
- Write-back cache: files appear instantly, sync to tape in background
- `_tape_status.txt` shows live status
- ls/cd returns instantly (from cache)
- Writes go to pending → sync thread → raw blocks → tape audio
- tkinter debug window with live waveform

### Hardware Connection (`tapeworm ls/format/read/write`)
- `--port /dev/ttyUSB0` for real ESP32
- `--port stdio` for testing with firmware sim
- All commands work through the packet protocol

## What's Broken / In Progress

### Modem Decoder (multi-tone OFDM)
The multi-tone decoder detects sync and collects frames, but the noise floor thresholds aren't calibrated. Encoder produces audio that the decoder can't reliably decode back to the original bits. Symptoms:
- `READ_NEXT` returns blocks, but data is wrong (modem decode errors)
- `read_file` returns `Error.INVALID` (−6)
- FUSE directory reads fail (can't decode tape blocks)

**Root cause:** The 4 simultaneous tones clip in the DAC (hard-clamped to [-1,1]), reducing per-tone SNR. The adaptive noise floor threshold needs calibration. Or switch to simpler FSK (1 tone at a time, higher SNR).

### FUSE Write-Back Sync
- Files written to mount go to pending cache (instant)
- Background sync writes raw blocks to firmware (encoder runs → audio generated)
- Directory is NOT stored on tape (decoder can't read it back)
- File list comes from in-memory cache only (lost on unmount)

### Hardware
- No real ESP32 testing done — all testing with firmware simulation
- MCP4725 DAC, PCM1808 ADC, cassette deck wiring not tested
- 3-wire motor control (FWD/REV/FAST) not tested on real hardware

## Files to Know

| File | Purpose |
|------|---------|
| `filesystem/tapeworm` | Main CLI — format, ls, read, write, mount |
| `filesystem/tapefs.py` | Filesystem library (CRC, RS, Directory, Block) |
| `filesystem/host_driver.py` | Python host driver (Esp32Connection, HostDriver) |
| `filesystem/dummy_mcu.py` | Python MCU simulator (for reference) |
| `filesystem/cpp/firmware/firmware.cpp` | ESP32 firmware main loop |
| `filesystem/cpp/firmware/modem_encoder.cpp` | Multi-tone OFDM encoder |
| `filesystem/cpp/firmware/modem_decoder.cpp` | Multi-tone OFDM decoder (needs tuning) |
| `filesystem/cpp/firmware/esp32_hal.cpp` | HAL — ADC, DAC, GPIO, Timer |
| `filesystem/cpp/firmware/tape_medium.cpp` | Raw file-backed tape storage |
| `filesystem/cpp/firmware/motor.cpp` | 3-wire motor model |
| `filesystem/cpp/firmware/tape_deck.cpp` | State machine |

## Quick Start

```bash
cd filesystem
./tapeworm format                    # format tape
./tapeworm mount ./tape_mount        # mount + UI
# In another terminal:
echo "hello" > tape_mount/test.txt   # instant write
cat tape_mount/test.txt              # reads from tape (slow)
tail -f /tmp/tapewormfs_debug.log /tmp/tapewormfs_fw.log  # debug
```
