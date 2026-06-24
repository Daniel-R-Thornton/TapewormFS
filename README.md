# 📡 Modem Workshop

A DSP toolkit for building software modems (PSK, OFDM, etc.)

## Structure
src/
├── core/
│ ├── DSPEngine.ts # FFT, signal generation, analysis
│ └── Modulator.ts # BPSK, QPSK, OFDM (you implement!)
├── visualization/
│ └── Visualizer.ts # Waveform, spectrogram, constellation
├── io/
│ └── AudioIO.ts # Playback, recording, WAV import
└── main.ts # Main application

text

## Quick Start

```bash
npm install
npm run dev```
