# TapewormFS

Store digital files on audio cassette tapes.

## Architecture

```
┌─────────────┐    ┌────────────────┐    ┌──────────────┐
│   /tmp      │───▶│  host-driver   │───▶│  firmware    │
│ (disk buf)  │    │  (FUSE/9P)     │    │  (ESP32/Pico)│
└─────────────┘    └────────────────┘    └──────┬───────┘
                       │                        │
                       ▼                        ▼
                ┌──────────────┐         ┌───────────┐
                │ debug-suite  │         │  cassette │
                │ (web UI)     │         │  (audio)  │
                └──────────────┘         └───────────┘
```

- **`firmware/`** — MCU drivers (ESP32, RP2040) talking to cassette deck over SPI
- **`filesystem/`** — Sequential tape filesystem (block alloc, error recovery)
- **`host-driver/`** — Local app that presents the tape as a sequential FS to the OS
- **`debug-suite/`** — Web-based modem debugger & waveform visualiser (Vite/TypeScript)
