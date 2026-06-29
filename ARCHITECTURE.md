# TapewormFS — Architecture Summary

## Concept
Store digital files on audio cassette tapes. The ESP32 emulates a USB flash drive so Windows/macOS sees it as a normal drive (`D:\`). Behind the scenes, files are actually encoded as audio and stored on cassette.

## Hardware

### Output path (write to tape)
```
ESP32 → I2C (MCP4725 12-bit DAC) → 3.5mm jack → cassette LINE IN
```

### Input path (read from tape)
```
Cassette LINE OUT → voltage divider + DC-block cap → ESP32 onboard ADC
```

## Software Architecture

```
┌──────────────────────────────────────────────────┐
│  User sees: D:\  (USB Mass Storage Class)        │
│  └── file.txt  (instant read/write)              │
│                                                   │
│  USB MSC via TinyUSB on ESP32-S3                 │
│  (12 Mbps USB 1.1, no drivers needed)            │
├──────────────────────────────────────────────────┤
│  SD Card Cache (FAT32)                           │
│  └── files/           ← what user sees           │
│  └── .tapecache/      ← sync metadata            │
│                                                   │
│  Reads/writes to SD happen instantly via SPI      │
│  (~4 MB/s at 40 MHz)                             │
├──────────────────────────────────────────────────┤
│  Background Tape Sync (dual-core)                 │
│  1. "Dirty" files on SD → encode → write to tape │
│  2. New blocks on tape → decode → copy to SD     │
│  3. Idle detection: syncs after 10s of no I/O    │
├──────────────────────────────────────────────────┤
│  Modem Layer (audio encode/decode)               │
│  └── BasicEncoder: phase-shift keying (~200 baud)│
│  └── FrequencyPulse: multi-tone FSK (~100 baud)  │
│  └── Both defined in debug-suite/ (TypeScript)   │
├──────────────────────────────────────────────────┤
│  Protocol Layer (host ↔ MCU)                     │
│  └── Framed binary packets over USB CDC:         │
│      [0xFE | len | cmd_id | payload | crc16]     │
│  └── Commands: WRITE_BLOCK, READ_NEXT, SEEK,     │
│      FLUSH, STOP, PING, GET_STATUS, SET_CONFIG   │
│  └── Byte escaping (0xFE→FD 01, FD→FD 02)       │
├──────────────────────────────────────────────────┤
│  Filesystem Layer (Python, on host for now)      │
│  └── tapefs.py: Directory, Block, RS(255,239),   │
│      CRC-32, Filesystem API                      │
│  └── dummy_mcu.py: Simulated ESP32 for testing   │
│  └── serial_transport.py: Real UART transport    │
│  └── All pure Python, no deps besides pyserial   │
└──────────────────────────────────────────────────┘
```

## Key Design Decisions

### Why not direct USB MSC with no SD card?
Tape is too slow (~25 B/s). Windows Explorer would time out on every directory listing. The SD card is the fast cache; tape is the long-term archive.

### Why ESP32-S3 (not basic ESP32)?
Needs native USB OTG for USB Mass Storage. ESP32-S2/S3 have it. Regular ESP32 doesn't — you'd need a separate USB bridge chip.

### Sync strategy
| Trigger | Action | Why |
|---------|--------|-----|
| User saves file | Write to SD instantly | Don't make user wait |
| 10s of no I/O | Start tape write | Background sync |
| User clicks "Eject" | Force-sync remaining, then safe-eject | Prevent data loss |
| Fresh tape inserted | Full tape scan → build SD cache | One-time wait (~30 min for C60) |

### RS(255,239) error correction
- Every 1024-byte block gets 16 parity bytes
- Corrects up to 8 corrupted bytes per block
- Encode + no-error decode works; error correction still being debugged
- CRC-32 wraps everything for integrity verification

### Performance targets
| Metric | Target |
|--------|--------|
| Raw bit rate on tape | 200 baud |
| Usable data rate | ~80 B/s (after ECC + framing) |
| C60 cassette capacity | ~180 KB/side |
| USB interface speed | 12 Mbps (USB 1.1, limited by ESP32) |
| SD card speed | ~4 MB/s (SPI at 40 MHz) |
| Block read time from tape | ~10 s |
| File open latency (cached) | <10 ms |
| File open latency (uncached) | 30–60 s (rewind + read dir) |

## Code Status

### Done
- `filesystem/tapefs.py` — full filesystem: CRC, RS encode/decode, Directory, Block serialisation, Filesystem API
- `filesystem/dummy_mcu.py` — simulated ESP32 with packet protocol, state machine, TCP server, error injection
- `filesystem/serial_transport.py` — real UART transport for pyserial
- `filesystem/test_tapefs.py` — 6 tests, all passing
- `debug-suite/` — web-based modem debugger (TypeScript/Vite)

### To Do
- USB MSC firmware on ESP32-S3 (TinyUSB + SD card + dual-core sync)
- Port modem from TypeScript → C for firmware
- USB CDC command handler (listen for WRITE_BLOCK/READ_NEXT etc.)
- Eject / force-sync UX (LED, button)
- First-tape-scan progress indicator

## How to run tests
```bash
cd filesystem
python3 test_tapefs.py
```
