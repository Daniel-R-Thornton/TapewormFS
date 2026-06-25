# TapewormFS

Store digital files on audio cassette tapes.

```mermaid
flowchart TB
    subgraph Host["Host Computer"]
        D["D:\ (USB Mass Storage)"]
        SD["SD Card Cache (FAT32)"]
        FS["tapefs::Filesystem"]
    end

    subgraph MCU["ESP32-S3 Firmware"]
        MSC["USB MSC (TinyUSB)"]
        PROTO["Packet Protocol<br/>[0xFE|len|cmd|payload|crc16]"]
        SYNC["Background Tape Sync"]
    end

    subgraph Tape["Cassette Deck"]
        DAC["MCP4725 (I2C) → output"]
        ADC["Onboard ADC → input"]
    end

    D <-- USB --> MSC
    MSC <--> SD
    SD <--> FS
    FS <--> PROTO
    PROTO <-- UART --> SYNC
    SYNC --> DAC
    ADC --> SYNC

    style Host fill:#e3f2fd
    style MCU fill:#f3e5f5
    style Tape fill:#fff3e0
```

## Project layout

| Path | What |
|------|------|
| `filesystem/cpp/` | C++17 production code (6 classes, 6 tests) |
| `filesystem/tapefs.py` | Python FS lib (for tests) |
| `filesystem/dummy_mcu.py` | ESP32 simulator with tape physics |
| `filesystem/test_tapefs.py` | Unit tests (6 pass) |
| `filesystem/test_integration.py` | Serial integration tests (5 pass) |
| `debug-suite/` | Web modem visualiser (TypeScript) |
| `SPEC.md` | Full protocol & filesystem spec |
| `OFDM_PHY.md` | Physical layer spec (FSK+pilot) |
| `ARCHITECTURE.md` | System architecture |

## Build & test

```bash
# C++ (production)
cd filesystem/cpp && mkdir build && cd build
cmake .. && make && ./test_tapefs

# Python (tests)
cd filesystem
python3 test_tapefs.py
python3 test_integration.py
```
