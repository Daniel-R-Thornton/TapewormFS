import { DSPEngine } from "./DSPEngine";

export interface StereoFrame {
  left: Float32Array;
  right: Float32Array;
}

/**
 * Symbols are grouped bits. The encoder receives one symbol at a time
 * and produces one full waveform frame for that symbol.
 */
abstract class Encoder {
  protected readonly dsp: DSPEngine;
  protected readonly sampleRate: number;
  protected readonly bitsPerSymbol: number;

  constructor(dsp: DSPEngine, sampleRate: number, bitsPerSymbol = 1) {
    this.dsp = dsp;
    this.sampleRate = sampleRate;
    this.bitsPerSymbol = Math.max(1, Math.floor(bitsPerSymbol));
  }

  abstract encodeSymbol(symbol: number): StereoFrame;

  protected *bytesToSymbols(data: Uint8Array): Iterable<number> {
    let pending = 0;
    let pendingBits = 0;

    for (const byte of data) {
      for (let i = 7; i >= 0; i--) {
        pending = (pending << 1) | ((byte >> i) & 1);
        pendingBits += 1;

        if (pendingBits >= this.bitsPerSymbol) {
          const shift = pendingBits - this.bitsPerSymbol;
          const symbol = pending >> shift;
          yield symbol;
          pending &= (1 << shift) - 1;
          pendingBits = shift;
        }
      }
    }

    if (pendingBits > 0) {
      yield pending << (this.bitsPerSymbol - pendingBits);
    }
  }

  protected *bytesToBits(data: Uint8Array): Iterable<number> {
    for (const byte of data) {
      for (let i = 7; i >= 0; i--) {
        yield (byte >> i) & 1;
      }
    }
  }

  protected bitsToBytes(bits: number[]): Uint8Array {
    const bytes: number[] = [];

    for (let i = 0; i < bits.length; i += 8) {
      let byte = 0;

      for (let j = 0; j < 8; j++) {
        byte = (byte << 1) | (bits[i + j] ?? 0);
      }

      bytes.push(byte);
    }

    return new Uint8Array(bytes);
  }
}

/* ---------------- MONO ---------------- */

export abstract class MonoEncoder extends Encoder {
  encode(data: Uint8Array): Float32Array {
    const out: Float32Array[] = [];

    for (const symbol of this.bytesToSymbols(data)) {
      const frame = this.encodeSymbol(symbol);
      out.push(frame.left); // mono uses left only
    }

    return this.concat(out);
  }

  protected concat(chunks: Float32Array[]): Float32Array {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Float32Array(total);

    let offset = 0;
    for (const c of chunks) {
      out.set(c, offset);
      offset += c.length;
    }

    return out;
  }
}

/* ---------------- STEREO ---------------- */

export abstract class StereoEncoder extends Encoder {
  encode(data: Uint8Array): StereoFrame {
    const left: Float32Array[] = [];
    const right: Float32Array[] = [];

    for (const symbol of this.bytesToSymbols(data)) {
      const frame = this.encodeSymbol(symbol);
      left.push(frame.left);
      right.push(frame.right);
    }

    return {
      left: this.concat(left),
      right: this.concat(right),
    };
  }

  protected concat(chunks: Float32Array[]): Float32Array {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Float32Array(total);

    let offset = 0;
    for (const c of chunks) {
      out.set(c, offset);
      offset += c.length;
    }

    return out;
  }
}
