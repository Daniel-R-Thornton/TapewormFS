# TapewormFS — Architecture Summary

Store files on audio cassette. The host PC runs a driver that mounts a
folder (e.g. `/mnt/tape`) backed by the cassette. The ESP32 handles only
modem encoding/decoding over UART.

---

## System Architecture

```mermaid
flowchart TB
    subgraph Host["Host Computer"]
        MOUNT["/mnt/tape0 (FUSE/WinFSP)<br/>looks like a normal folder"]
        CACHE["/tmp/tapewormfs/<br/>disk-backed cache"]
        FS["tapefs::Filesystem"]
        DRIVER["Host Driver<br/>(background sync thread)"]
        PROTO["Packet Protocol<br/>[0xFE | len | cmd | payload | crc16]"]
    end

    subgraph ESP32["ESP32 (any model)"]
        UART["UART command handler"]
        MODEM["FSK Modem + Pilot"]
        DAC["MCP4725 (I2C) → cassette LINE IN"]
        ADC["Onboard ADC ← cassette LINE OUT"]
    end

    subgraph Tape["Cassette Deck"]
        MEDIA["Audio Cassette"]
    end

    User["User copies files<br/>to /mnt/tape0"] --> MOUNT
    MOUNT <--> CACHE
    CACHE <--> FS
    FS <--> DRIVER
    DRIVER <--> PROTO
    PROTO <-- UART --> UART
    UART --> MODEM
    MODEM --> DAC --> MEDIA
    MEDIA --> ADC --> MODEM

    style Host fill:#e3f2fd
    style ESP32 fill:#f3e5f5
    style Tape fill:#fff3e0
```

## Data Flow

```mermaid
sequenceDiagram
    participant User as User App
    participant Cache as /tmp cache
    participant FS as Filesystem
    participant Proto as Packet Protocol
    participant ESP as ESP32
    participant Tape as Cassette

    Note over User,Tape: Write file to /mnt/tape0/doc.txt
    User->>Cache: write(file, data)  (instant)
    Cache-->>FS: mark file as dirty

    Note over FS,Tape: Background sync (seconds later)
    FS->>FS: split into blocks
    FS->>FS: add RS parity + CRC
    FS->>Proto: WRITE_BLOCK + FLUSH
    Proto->>ESP: [0xFE|len|0x05|payload|crc16]
    ESP->>ESP: FSK encode → audio samples
    ESP->>Tape: output via MCP4725

    Note over User,Tape: Read file from /mnt/tape0/doc.txt
    Cache-->>FS: cache miss
    FS->>Proto: SEEK + READ_NEXT
    Proto->>ESP: [0xFE|len|0x06|crc16]
    ESP->>ESP: decode audio ← ADC
    ESP-->>Proto: raw block back
    FS->>FS: verify CRC, reassemble
    FS->>Cache: cache file
    Cache->>User: return data
```

## Sync Strategy

| Trigger | Action |
|---------|--------|
| User saves file to mount | Write to /tmp cache instantly |
| 10s of no I/O | Start background write to tape |
| User reads uncached file | Read from tape (slow), cache result |
| File already cached | Serve from /tmp instantly |
| Eject / unmount | Force-sync all dirty files to tape |

## Project Layout

```
TapewormFS/
├── filesystem/
│   ├── tapefs.py              ← Python FS lib (for tests)
│   ├── dummy_mcu.py           ← ESP32 simulator
│   ├── host_driver.py         ← Mounts a folder backed by tape
│   ├── test_tapefs.py         ← Unit tests (6 pass)
│   ├── test_integration.py    ← Integration tests (5 pass)
│   └── cpp/                   ← C++17 production code
│       ├── CMakeLists.txt
│       ├── include/tapefs/    ← Headers
│       ├── src/               └── Implementation
│       └── tests/             ← C++ unit tests (6 pass)
├── SPEC.md                    ← Full spec
└── OFDM_PHY.md                ← Physical layer spec
```

Run tests:
```bash
cd filesystem
python3 test_tapefs.py
python3 test_integration.py
cd cpp/build && cmake .. && make && ./test_tapefs
```
