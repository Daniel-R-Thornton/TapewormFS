import { DSPEngine } from "../DSPEngine";
import { MonoDecoder } from "../Decoder";
import { MonoEncoder } from "../Encoder";
import { MonoFrame } from "../Types";

export type WaveformShape = "sine" | "square" | "triangle" | "sawtooth";

export interface BasicProcessorOptions {
  pulseFrequency?: number;
  frameSize?: number;
  waveformShape?: WaveformShape;
}

/* ---------------- MONO ---------------- */
// A simple encoder that emits a tone for a 1 bit and an inverted tone for a 0 bit.
// The decoder simply sums the received frame and decides the bit polarity.
export class BasicEncoder extends MonoEncoder {
  private readonly pulseFrequency: number;
  private readonly frameSize: number;
  private readonly waveformShape: WaveformShape;

  constructor(
    dsp: DSPEngine,
    sampleRate: number,
    options: BasicProcessorOptions = {},
  ) {
    super(dsp, sampleRate);
    this.pulseFrequency = options.pulseFrequency ?? 1000;
    this.frameSize = options.frameSize ?? 1024;
    this.waveformShape = options.waveformShape ?? "sine";
  }

  private oscillatorValue(index: number, frequency: number): number {
    const cycles = (index * frequency) / this.sampleRate;
    const phase = cycles - Math.floor(cycles);
    switch (this.waveformShape) {
      case "square":
        return phase < 0.5 ? 1 : -1;
      case "sawtooth":
        return 2 * phase - 1;
      case "triangle":
        return 1 - 4 * Math.abs(phase - 0.5);
      case "sine":
      default:
        return Math.sin(2 * Math.PI * phase);
    }
  }

  encodeSymbol(bit: number): MonoFrame {
    const frame = new Float32Array(this.frameSize);
    for (let i = 0; i < frame.length; i++) {
      const carrier = this.oscillatorValue(i, this.pulseFrequency);
      frame[i] = bit === 1 ? carrier : -carrier;
    }
    return {
      left: frame,
      right: frame,
    };
  }
}

export class BasicDecoder extends MonoDecoder {
  private readonly frameSize: number;
  constructor(
    dsp: DSPEngine,
    sampleRate: number,
    options: BasicProcessorOptions = {},
  ) {
    super(dsp, sampleRate);
    this.frameSize = options.frameSize ?? 1024;
  }

  protected estimateFrameSize(): number {
    return this.frameSize;
  }

  public decodeSymbol(frame: MonoFrame): number {
    const sum = frame.left.reduce((acc, val) => acc + val, 0);
    return sum > 0 ? 1 : 0;
  }
}
