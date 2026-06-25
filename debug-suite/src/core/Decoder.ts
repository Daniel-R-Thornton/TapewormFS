import { DSPEngine } from "./DSPEngine";
import { StereoFrame } from "./Encoder";

export interface DecodeResult {
  bytes: Uint8Array;
  debug?: any;
}

/**
 * Decoder works on full symbol frames.
 * It does NOT care about pip timing.
 */
abstract class Decoder {
  protected readonly dsp: DSPEngine;
  protected readonly sampleRate: number;
  protected readonly bitsPerSymbol: number;

  constructor(dsp: DSPEngine, sampleRate: number, bitsPerSymbol = 1) {
    this.dsp = dsp;
    this.sampleRate = sampleRate;
    this.bitsPerSymbol = Math.max(1, Math.floor(bitsPerSymbol));
  }

  /**
   * Decode a FULL waveform frame into one symbol.
   */
  abstract decodeSymbol(frame: StereoFrame): number;

  protected symbolToBits(symbol: number): number[] {
    const bits: number[] = [];
    for (let i = this.bitsPerSymbol - 1; i >= 0; i--) {
      bits.push((symbol >> i) & 1);
    }
    return bits;
  }

  protected symbolsToBytes(symbols: number[]): Uint8Array {
    const bits: number[] = [];
    for (const symbol of symbols) {
      bits.push(...this.symbolToBits(symbol));
    }
    return this.bitsToBytes(bits);
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

export abstract class MonoDecoder extends Decoder {
  decode(signal: Float32Array): DecodeResult {
    const symbols: number[] = [];

    let offset = 0;
    const frameSize = this.estimateFrameSize();

    while (offset + frameSize <= signal.length) {
      const frame = signal.slice(offset, offset + frameSize);

      const symbol = this.decodeSymbol({
        left: frame,
        right: frame,
      });

      symbols.push(symbol);
      offset += frameSize;
    }

    return {
      bytes: this.symbolsToBytes(symbols),
    };
  }

  /**
   *  this may come from metadata or DSP estimation.
   */
  protected abstract estimateFrameSize(): number;
}

/* ---------------- STEREO ---------------- */

export abstract class StereoDecoder extends Decoder {
  decode(signal: StereoFrame): DecodeResult {
    const symbols: number[] = [];

    let offset = 0;
    const frameSize = this.estimateFrameSize();

    while (offset + frameSize <= signal.left.length) {
      const frame: StereoFrame = {
        left: signal.left.slice(offset, offset + frameSize),
        right: signal.right.slice(offset, offset + frameSize),
      };

      const symbol = this.decodeSymbol(frame);

      symbols.push(symbol);
      offset += frameSize;
    }

    return {
      bytes: this.symbolsToBytes(symbols),
    };
  }

  protected abstract estimateFrameSize(): number;
}
