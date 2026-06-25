# TapewormFS — Specification

Store digital files on standard audio cassette tapes.

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                      HOST COMPUTER                       │
│  ┌──────────┐    ┌──────────────┐    ┌───────────────┐  │
│  │  /tmp/   │◀──▶│ host-driver  │◀──▶│  debug-suite  │  │
│  │ disk buf │    │ (FUSE daemon)│    │  (web UI)     │  │
│  └──────────┘    └──────┬───────┘    └───────────────┘  │
│                          │ USB/UART                      │
└──────────────────────────┼──────────────────────────────┘
                           │
                    ┌──────┴───────┐
                    │   MCU        │
                    │ (ESP32/Pico) │
                    └──────┬───────┘
                           │ SPI / analog
                    ┌──────┴───────┐
                    │ Cassette     │
                    │ Deck         │
                    └──────────────┘
```

### 1.1 Components

| Layer | Tech | Role |
|-------|------|------|
| **host-driver** | Rust / C (FUSE) | Presents tape as sequential file system to OS |
| **debug-suite** | TypeScript / Vite | Web-based waveform debugger & visualiser |
| **firmware** | C / ESP-IDF / Pico SDK | Bit-level encode/decode to audio via SPI DAC/ADC |
| **filesystem** | Rust / C | Block allocation, error recovery, directory structure |

---

## 2. Data Flow

### 2.1 Write Path

```
User file  →  host-driver  →  /tmp buffer  →  filesystem layer
      ↓
  blocks with ECC
      ↓
  modem encode (FreqShift / Basic)
      ↓
  firmware → SPI DAC → cassette LINE IN
```

### 2.2 Read Path

```
Cassette LINE OUT → firmware (ADC) → modem decode
      ↓
  raw blocks with error flags
      ↓
  filesystem layer → ECC recovery → reassemble
      ↓
  /tmp buffer → host-driver → OS
```

---

## 3. Transport Protocol (MCU ↔ Host)

### 3.1 Physical Layer

| Connection | Speed | Notes |
|------------|-------|-------|
| USB CDC (serial) | 115200–921600 baud | Primary debug/production link |
| SPI | 1–10 MHz | Direct MCU↔MCU, or MCU↔SBC |

### 3.2 Packet Format (binary, little-endian)

```
┌────┬──────┬────────┬──────────┬──────────┐
│0xFE│ len  │ cmd_id │ payload  │ checksum │
│ 1B │ 2B   │ 1B     │ 0-65535B │ CRC-16   │
└────┴──────┴────────┴──────────┴──────────┘
```

- **Start marker:** `0xFE`
- **Length:** payload length (excludes header, includes checksum)
- **Command ID:** see §3.3
- **Payload:** command-specific
- **Checksum:** CRC-16-IBM of everything after start marker

### 3.3 Commands

| ID | Name | Direction | Payload | Response |
|----|------|-----------|---------|----------|
| `0x01` | `PING` | host→MCU | empty | `0x81` + firmware version string |
| `0x02` | `WRITE_BLOCK` | host→MCU | block data (≤1024 B) | `0x82` + ACK/NACK |
| `0x03` | `READ_BLOCK` | host→MCU | block address (4B) | `0x83` + block data / error |
| `0x04` | `GET_STATUS` | host→MCU | empty | `0x84` + tape status flags |
| `0x05` | `STOP` | host→MCU | empty | `0x85` + ACK |
| `0x06` | `REWIND` | host→MCU | empty | `0x86` + ACK (non-blocking) |
| `0x07` | `SET_SPEED` | host→MCU | speed enum (1B) | `0x87` + ACK |
| `0x08` | `STREAM_MODE` | host→MCU | mode enum (1B) | `0x88` + ACK |

### 3.4 Status Flags (bitmask, GET_STATUS response)

| Bit | Flag | Meaning |
|-----|------|---------|
| 0 | `TAPE_PRESENT` | Cassette detected in deck |
| 1 | `TAPE_MOVING` | Motor engaged |
| 2 | `WRITE_PROTECT` | Tab broken |
| 3 | `ENCODING` | Currently encoding/writing |
| 4 | `DECODING` | Currently decoding/reading |
| 5 | `BUFFER_FULL` | MCU buffer at capacity |
| 6 | `ERROR` | General error state |
| 7 | `EOT` | End of tape detected |

---

## 4. Physical Cassette Encoding

### 4.1 Audio Modulation (Modem Layer)

| Scheme | Bit rate | Notes |
|--------|----------|-------|
| **Basic** (BPSK-like) | ~100–300 baud | Simple carrier phase shift |
| **FrequencyPulse** (FSK) | ~50–200 baud | Multi-tone FSK, more robust |

Both defined in `debug-suite/src/core/processors/`.

### 4.2 Cassette Tape Layout

```
┌─────────────────────────────────────────────────────┐
│ Leader │  Sync  │ Block 1 │ Block 2 │ ... │ Trailer │
│ (5s)   │ (512B) │ (<=1KB) │ (<=1KB) │     │ (5s)    │
└─────────────────────────────────────────────────────┘
```

- **Leader:** Unmodulated carrier / silence for AGC stabilisation
- **Sync:** Fixed preamble (`0xFE 0xED 0xBE 0xEF` repeated) for block alignment
- **Blocks:** See §5
- **Trailer:** End-of-tape marker + silence

### 4.3 Inter-Block Gap

≥ 500 ms of silence between blocks to allow the cassette deck's AGC to reset and the MCU to process.

---

## 5. Filesystem Format

### 5.1 Block Structure

Each physical block on tape (≤1024 bytes after modem encode):

```
┌────┬──────────┬──────────┬──────────────┬────────┐
│  B │  seq_no  │  payload │  ECC (Reed-  │ CRC-32 │
│  E │  (4B)    │  (≤1000B)│  Solomon)    │  (4B)   │
│  A │          │          │  (≤16B)      │        │
│  D │          │          │              │        │
│  E │          │          │              │        │
│  R │          │          │              │        │
└────┴──────────┴──────────┴──────────────┴────────┘
```

**Prefix byte** — block type:
- `0x01` = data block
- `0x02` = directory block
- `0x03` = FAT block (file allocation table)
- `0x04` = ECC / parity block
- `0xFF` = end-of-tape marker

### 5.2 Directory

Stored as a single block near start of tape (rewound to find).

```
┌──────────┬────────────┬─────────────┬────────────┐
│  magic   │  file_1     │  file_2      │  ...       │
│  "TWF"   │  entry      │  entry       │            │
│  (3B)    │  (32B each) │  (32B each)  │            │
└──────────┴────────────┴─────────────┴────────────┘
```

Each file entry (32 bytes):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 20 | Filename (null-padded ASCII) |
| 20 | 4 | Start block number |
| 24 | 4 | End block number |
| 28 | 4 | File size in bytes |

### 5.3 Error Recovery Strategy

Because audio cassette is _very_ error-prone:

1. **Every block has Reed-Solomon ECC** — corrects up to 8 byte errors per block
2. **Block-level interleaving** — consecutive blocks are not adjacent on tape (spread over ~10s of tape to handle dropouts)
3. **Redundant FAT** — two copies of directory/FAT at start of tape
4. **Read retries** — MCU re-reads failed blocks up to 3 times with different AGC settings
5. **Partial reads** — filesystem can return what it got + bitmap of lost blocks

---

## 6. /tmp Buffer (Host-Driver)

Since cassette I/O is far slower than disk, the host-driver maintains a disk-backed buffer:

### 6.1 Write Buffer

```
User writes file
    ↓
host-driver stores in /tmp/tapewormfs/<session>/write_buf/
    ↓
Background thread encodes blocks and sends to MCU
    ↓
On success, blocks are freed from buffer
```

### 6.2 Read Buffer

```
User requests file
    ↓
host-driver checks /tmp/tapewormfs/<session>/read_buf/
    ↓
If not cached: signals MCU to start reading tape
    ↓
Blocks arrive slowly, appended to read_buf/
    ↓
OS sees a sequential stream that blocks on read()
```

### 6.3 Session Management

Each tape load/eject gets a new session ID:

```
/tmp/tapewormfs/
├── 2026-06-25_104518/
│   ├── write_buf/
│   ├── read_buf/
│   └── manifest.json
└── current -> 2026-06-25_104518  (symlink)
```

---

## 7. Host-Driver OS Integration

### 7.1 FUSE Implementation

Presents as mount point e.g. `/mnt/tape0`.

| Operation | Behaviour |
|-----------|-----------|
| `open("/mnt/tape0/myfile.wav")` | Looks up in tape directory, starts streaming read |
| `read(fd, buf, n)` | Returns buffered data; blocks if buffer empty (MCU still reading) |
| `write(fd, buf, n)` | Appends to write buffer; returns immediately |
| `close(fd)` | Flushes write buffer, starts encode+write to tape |
| `readdir("/mnt/tape0")` | Reads directory block from tape |
| `stat("/mnt/tape0")` | Returns tape metadata (capacity, blocks free) |

### 7.2 Non-FUSE Fallback

A simple TCP server (port 9725) that speaks a text protocol for systems that can't run FUSE:

```
> LIST
< file1.txt 1024 3
< file2.bin 8192 24
> READ file1.txt
< 1024 bytes follow
> WRITE myfile.dat 4096
< OK send 4096 bytes
```

---

## 8. Implementation Phases

### Phase 1 — Core Modem (done)
- [x] BasicEncoder with waveform shape selection
- [x] FrequencyPulse (FSK) encoder/decoder
- [x] DSPEngine, frame sync, correlation scoring
- [x] Web debug UI with live waveform visualiser

### Phase 2 — Filesystem Layer
- [ ] Block format + serialisation
- [ ] Directory read/write
- [ ] Reed-Solomon ECC
- [ ] Read retry logic
- [ ] Partial read support

### Phase 3 — Firmware (ESP32)
- [ ] Serial protocol implementation
- [ ] SPI DAC output (audio generation)
- [ ] SPI ADC input (audio capture)
- [ ] Real-time encode/decode on MCU
- [ ] Motor control (relay / MOSFET)

### Phase 4 — Firmware (RP2040)
- [ ] Same as Phase 3, Pico SDK variant
- [ ] PIO-based DAC/ADC if available

### Phase 5 — Host-Driver
- [ ] FUSE daemon (Rust)
- [ ] /tmp buffer management
- [ ] Session persistence
- [ ] TCP fallback server
- [ ] Blocks encode/decode pipeline

### Phase 6 — Integration & Testing
- [ ] End-to-end write: OS → buffer → firmware → cassette
- [ ] End-to-end read: cassette → firmware → buffer → OS
- [ ] Error injection testing (dropouts, noise)
- [ ] Performance tuning (baud rate vs reliability)
- [ ] Real cassette deck hardware testing

---

## 9. Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| Raw bit rate | 200 baud | C60 = 60 min side → ~90 KB/side |
| Usable data rate | ~80 baud | After ECC + framing overhead |
| C60 capacity | ~180 KB/side | ~360 KB per C60 cassette |
| C90 capacity | ~270 KB/side | ~540 KB per C90 cassette |
| C120 capacity | ~360 KB/side | ~720 KB per C120 (thinner, riskier) |
| Block read time | ~10 s/block | At 200 baud, 1 KB block |
| File open latency | ~30–60 s | Rewind + read directory block |

---

## 10. Glossary

| Term | Meaning |
|------|---------|
| AGC | Automatic Gain Control (in cassette deck) |
| BPSK | Binary Phase Shift Keying |
| ECC | Error Correcting Code |
| FAT | File Allocation Table |
| FSK | Frequency Shift Keying |
| FUSE | Filesystem in Userspace |
| MCU | Microcontroller Unit |
| Reed-Solomon | Block-based ECC algorithm |
