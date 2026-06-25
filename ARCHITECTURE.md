# TapewormFS — Architecture Summary

Concept: store digital files on audio cassette. The ESP32-S3 presents as a
USB flash drive; an SD card provides instant I/O; tape syncs in background.

---

## System Architecture

```mermaid
flowchart TB
    subgraph User[" "]
        D["D:\ (USB MSC)"]
    end

    subgraph ESP32["ESP32-S3 Firmware"]
        USB["USB Mass Storage<br/>(TinyUSB)"]
        SD["SD Card Cache<br/>(FAT32)"]
        FS["Filesystem Layer<br/>(format, write, read)"]
        SYNC["Background Sync<br/>(tape ↔ SD)"]
        MODEM["Modem<br/>(FSK + Pilot)"]
        PROTO["Packet Protocol<br/>[0xFE | len | cmd | payload | crc16]"]

        USB <--> SD
        SD <--> FS
        FS <--> SYNC
        SYNC <--> MODEM
        MODEM <--> PROTO
    end

    subgraph Tape["Cassette Deck"]
        DAC["MCP4725 DAC<br/>(I2C)"]
        ADC["Onboard ADC"]
        TAPE["Audio Cassette"]
        DAC --> TAPE
        TAPE --> ADC
    end

    User <-- USB --> USB
    PROTO <-- UART --> DAC
    ADC <-- UART --> PROTO

    style User fill:#e1f5fe
    style ESP32 fill:#f3e5f5
    style Tape fill:#fff3e0
```

## Data Flow

```mermaid
sequenceDiagram
    participant User as User App
    participant Cache as SD Cache
    participant FS as Filesystem
    participant Modem as FSK Modem
    participant Tape as Cassette

    Note over User,Tape: Write Path
    User->>Cache: Save file (instant)
    User->>FS: write_file("doc.txt")
    FS->>FS: split into blocks
    FS->>FS: add RS parity + CRC
    FS->>Modem: encode blocks to audio
    Modem->>Tape: output via MCP4725
    
    Note over User,Tape: Read Path
    Tape->>Modem: audio input via ADC
    Modem->>Modem: decode FSK tones
    Modem->>FS: raw blocks
    FS->>FS: verify CRC, correct RS
    FS->>FS: reassemble file
    FS->>Cache: cache for next time
    Cache->>User: return data
    
    Note over User,Tape: Background Sync
    Cache-->>FS: dirty file detected
    FS-->>Modem: encode to audio
    Modem-->>Tape: write to cassette
```

## Performance

| Metric | Target |
|--------|--------|
| Raw bit rate | 200 baud |
| Net throughput | ~100 B/s (after FEC) |
| C60 capacity | ~180 KB/side |
| USB interface | 12 Mbps (USB 1.1) |
| SD card speed | ~4 MB/s (SPI) |
| Block read from tape | ~10 s |
| File open (cached) | <10 ms |
| File open (uncached) | 30–60 s |

## Project Layout

```
TapewormFS/
├── filesystem/
│   ├── tapefs.py              ← Python FS lib (for tests)
│   ├── dummy_mcu.py           ← ESP32 simulator (stdio mode)
│   ├── test_tapefs.py         ← Unit tests (6 pass)
│   ├── test_integration.py    ← Integration tests (5 pass)
│   └── cpp/                   ← C++17 production code
│       ├── CMakeLists.txt
│       ├── include/tapefs/    ← Headers (7 files)
│       ├── src/               ← Implementation (6 files)
│       └── tests/             ← C++ unit tests (6 pass)
├── SPEC.md                    ← Full spec
├── OFDM_PHY.md                ← Physical layer spec
├── CPP_STYLE.md               ← C++ style guide
└── debug-suite/               ← Web modem visualiser
```

Run tests:
```bash
cd filesystem
python3 test_tapefs.py         # unit tests
python3 test_integration.py    # integration tests
cd cpp/build && cmake .. && make && ./test_tapefs
```
