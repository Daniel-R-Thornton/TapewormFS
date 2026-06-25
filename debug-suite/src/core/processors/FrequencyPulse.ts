import { DSPEngine } from "../DSPEngine";
import { DecodeResult, MonoDecoder } from "../Decoder";
import { MonoEncoder } from "../Encoder";

export type WaveformShape = "sine" | "square" | "triangle" | "sawtooth";

export interface FrequencyPulseOptions {
  pulseFrequency?: number;
  frequencyStep?: number;
  frameSize?: number;
  symbolLength?: number;
  bitsPerFrame?: number;
  bitsPerSymbol?: number;
  // waveformShape restricted to sine-only (frequency detection requires sinusoids)
  /** Absolute floor for tone detection (scores are typically ~1–4). */
  detectionThreshold?: number;
  fftPadFactor?: number;
  localBinWindow?: number;
  /** Goertzel handles overlapping in-frame tones more reliably than FFT bins. */
  useGoertzel?: boolean;
  /** Scan for frame alignment (needed for padded WAV captures). Default off for round-trip. */
  searchSync?: boolean;
  debug?: boolean;
}

/* =========================
   ENCODER
========================= */

export class FrequencyPulseEncoder extends MonoEncoder {
  private readonly baseFrequency: number;
  private readonly frequencyStep: number;
  private readonly frameSize: number;
  private readonly stepSize: number;
  private readonly envelope: Float32Array;
  private readonly waveformShape: WaveformShape;

  constructor(
    dsp: DSPEngine,
    sampleRate: number,
    options: FrequencyPulseOptions = {},
  ) {
    const bitsPerSymbol = Math.max(
      1,
      Math.floor(options.bitsPerSymbol ?? options.bitsPerFrame ?? 8),
    );
    super(dsp, sampleRate, bitsPerSymbol);

    this.baseFrequency = options.pulseFrequency ?? 10000;
    this.frequencyStep = options.frequencyStep ?? 1000;
    this.frameSize = options.symbolLength ?? options.frameSize ?? 1024;
    this.stepSize = this.frameSize;
    this.waveformShape = "sine"; // Frequency detection requires sinusoids

    this.envelope = this.createEnvelope(this.frameSize);
  }

  private oscillatorValue(freq: number, sampleIndex: number): number {
    const cycles = (sampleIndex * freq) / this.sampleRate;
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

  private generateFrame(symbol: number, startSample: number): Float32Array {
    const frame = new Float32Array(this.frameSize);

    for (let bitIndex = 0; bitIndex < this.bitsPerSymbol; bitIndex++) {
      const bit = (symbol >> bitIndex) & 1;
      if (!bit) continue;

      const freq = this.baseFrequency + bitIndex * this.frequencyStep;
      for (let i = 0; i < this.frameSize; i++) {
        frame[i] +=
          this.oscillatorValue(freq, startSample + i) * this.envelope[i];
      }
    }

    return frame;
  }

  encode(data: Uint8Array): Float32Array {
    const symbols = Array.from(this.bytesToSymbols(data));
    const frames = Math.max(1, symbols.length);
    const totalSamples = frames * this.stepSize;

    const output = new Float32Array(totalSamples);

    for (let frameIndex = 0; frameIndex < frames; frameIndex++) {
      const frameStart = frameIndex * this.stepSize;
      const symbol = symbols[frameIndex] ?? 0;
      const frame = this.generateFrame(symbol, frameStart);
      output.set(frame, frameStart);
    }

    let max = 0;
    for (let i = 0; i < output.length; i++) {
      max = Math.max(max, Math.abs(output[i]));
    }

    if (max > 0) {
      for (let i = 0; i < output.length; i++) {
        output[i] /= max;
      }
    }

    return output;
  }

  encodeSymbol(symbol: number) {
    const frame = this.generateFrame(symbol, 0);
    return { left: frame, right: frame };
  }

  private createEnvelope(length: number): Float32Array {
    const ones = new Float32Array(length);
    ones.fill(1);
    return this.dsp.applyWindow(ones, "hann");
  }
}

/* =========================
   DECODER (WITH DEBUG)
========================= */

export class FrequencyPulseDecoder extends MonoDecoder {
  private readonly baseFrequency: number;
  private readonly frequencyStep: number;
  private readonly frameSize: number;
  private readonly stepSize: number;
  private readonly detectionThreshold: number;
  private readonly fftPadFactor: number;
  private readonly localBinWindow: number;
  private readonly useGoertzel: boolean;
  private readonly searchSync: boolean;

  private readonly debug: boolean;

  constructor(
    dsp: DSPEngine,
    sampleRate: number,
    options: FrequencyPulseOptions = {},
  ) {
    const bitsPerSymbol = Math.max(
      1,
      Math.floor(options.bitsPerSymbol ?? options.bitsPerFrame ?? 8),
    );
    super(dsp, sampleRate, bitsPerSymbol);

    this.baseFrequency = options.pulseFrequency ?? 10000;
    this.frequencyStep = options.frequencyStep ?? 1000;
    this.frameSize = options.frameSize ?? 1024;
    this.stepSize = this.frameSize;

    this.detectionThreshold = options.detectionThreshold ?? 1.05;
    this.fftPadFactor = Math.max(1, Math.floor(options.fftPadFactor ?? 1));
    this.localBinWindow = Math.max(1, Math.floor(options.localBinWindow ?? 3));
    this.useGoertzel = options.useGoertzel ?? true;
    this.searchSync = options.searchSync ?? false;
    this.debug = !!options.debug;
  }

  private frequencyForBit(bitIndex: number): number {
    return this.baseFrequency + bitIndex * this.frequencyStep;
  }

  /** Adaptive threshold handles sparse vs dense overlapping tones in one frame. */
  private symbolFromScores(scores: number[]): number {
    const n = scores.length;
    if (n === 0) return 0;

    let min = Infinity;
    let max = -Infinity;
    for (const score of scores) {
      min = Math.min(min, score);
      max = Math.max(max, score);
    }

    const spread = max - min;

    if (max < this.detectionThreshold) {
      return 0;
    }

    // Dense FDM frame: every tone present, scores cluster together.
    if (spread < 0.35 && max >= this.detectionThreshold) {
      if (min >= max * 0.9) {
        return (1 << n) - 1;
      }

      let symbol = 0;
      const denseCutoff = min + spread * 0.5;
      for (let bitIndex = 0; bitIndex < n; bitIndex++) {
        if (scores[bitIndex] >= denseCutoff) {
          symbol |= 1 << bitIndex;
        }
      }
      return symbol;
    }

    // Active carriers sit near max; leakage stays well below max * 0.82.
    const cutoff = Math.max(
      this.detectionThreshold,
      max * 0.82,
      min + spread * 0.5,
    );

    let symbol = 0;
    for (let bitIndex = 0; bitIndex < n; bitIndex++) {
      if (scores[bitIndex] >= cutoff) {
        symbol |= 1 << bitIndex;
      }
    }
    return symbol;
  }

  private scoreFrame(frame: Float32Array): number[] {
    const scores: number[] = [];
    for (let bitIndex = 0; bitIndex < this.bitsPerSymbol; bitIndex++) {
      scores.push(
        this.frequencyCorrelation(frame, this.frequencyForBit(bitIndex)),
      );
    }
    return scores;
  }

  private frameSyncScore(frame: Float32Array): number {
    const scores = this.scoreFrame(frame);
    const sorted = [...scores].sort((a, b) => b - a);
    const topCount = Math.max(1, Math.ceil(this.bitsPerSymbol / 2));
    let sum = 0;
    for (let i = 0; i < topCount; i++) {
      sum += sorted[i] ?? 0;
    }
    return sum;
  }

  protected estimateFrameSize(): number {
    return this.frameSize;
  }

  /** Decode PCM assuming optional sample offset (defaults to sync search). */
  public decodeAtOffset(signal: Float32Array, offset: number): DecodeResult {
    const res = this.decodeFromOffset(signal, offset);
    res.debug = { frameOffset: offset, ...(res.debug ?? {}) };
    return res;
  }

  public decode(signal: Float32Array): DecodeResult {
    const offset = this.searchSync ? this.findBestFrameOffset(signal) : 0;

    if (this.debug) {
      console.log("\n[SYNC]");
      console.log("chosen offset:", offset);
    }

    const res = this.decodeFromOffset(signal, offset);

    if (this.debug) {
      console.log("\n[RESULT]");
      console.log("frames decoded:", res.debug.frameCount);
    }

    res.debug = {
      frameOffset: offset,
      ...(res.debug ?? {}),
    };

    return res;
  }

  /** Weakest aligned frame — misaligned offsets dip when straddling Hann nulls. */
  private offsetMetric(offset: number, signal: Float32Array): number {
    const framesAvailable = Math.floor(
      (signal.length - offset) / this.stepSize,
    );
    if (framesAvailable < 1) return -Infinity;

    const frameCount = Math.min(framesAvailable, 8);
    let minFrameScore = Infinity;
    let activeFrames = 0;

    for (let f = 0; f < frameCount; f++) {
      const start = offset + f * this.stepSize;
      const frameScore = this.frameSyncScore(
        signal.slice(start, start + this.frameSize),
      );
      if (frameScore < this.detectionThreshold * 0.6) {
        continue;
      }
      activeFrames += 1;
      minFrameScore = Math.min(minFrameScore, frameScore);
    }

    if (activeFrames === 0) {
      return -Infinity;
    }

    return minFrameScore;
  }

  private findBestFrameOffset(signal: Float32Array): number {
    const maxOffset = Math.min(this.frameSize, signal.length);

    let bestOffset = 0;
    let bestMetric = -Infinity;

    for (let offset = 0; offset < maxOffset; offset++) {
      const metric = this.offsetMetric(offset, signal);

      if (this.debug && offset < 5) {
        console.log(`[OFFSET ${offset}] metric =`, metric.toFixed(2));
      }

      const tieBand = 0.02;
      if (
        metric > bestMetric + tieBand ||
        (Math.abs(metric - bestMetric) <= tieBand && offset < bestOffset)
      ) {
        bestMetric = metric;
        bestOffset = offset;
      }
    }

    return bestOffset;
  }

  private decodeFromOffset(signal: Float32Array, offset: number): DecodeResult {
    const symbols: number[] = [];
    const scores: number[][] = [];

    const frames = Math.floor((signal.length - offset) / this.stepSize);

    for (let f = 0; f < frames; f++) {
      const start = offset + f * this.stepSize;
      if (start + this.frameSize > signal.length) break;

      const frame = signal.slice(start, start + this.frameSize);
      const frameScores = this.scoreFrame(frame);
      const symbol = this.symbolFromScores(frameScores);

      if (this.debug && f < 10) {
        console.log(`\n[FRAME ${f}]`);
        console.log(
          "scores:",
          frameScores.map((s) => s.toFixed(2)),
        );
        console.log("symbol:", symbol);
      }

      symbols.push(symbol);
      scores.push(frameScores);
    }

    return {
      bytes: this.symbolsToBytes(symbols),
      debug: {
        frameCount: symbols.length,
        scores,
        symbols,
      },
    };
  }

  public decodeSymbol(frame: { left: Float32Array }): number {
    return this.symbolFromScores(this.scoreFrame(frame.left));
  }

  private isOtherCarrierBin(
    bin: number,
    targetBin: number,
    fftSize: number,
  ): boolean {
    for (let bitIndex = 0; bitIndex < this.bitsPerSymbol; bitIndex++) {
      const carrierBin = Math.round(
        (this.frequencyForBit(bitIndex) * fftSize) / this.sampleRate,
      );
      if (Math.abs(carrierBin - targetBin) <= 1) continue;
      if (Math.abs(bin - carrierBin) <= this.localBinWindow) {
        return true;
      }
    }
    return false;
  }

  private frequencyCorrelation(frame: Float32Array, freq: number): number {
    const n = frame.length;
    const windowed = this.dsp.applyWindow(frame.slice(0, n), "hann");

    const eps = 1e-12;

    if (this.useGoertzel) {
      const resolution = this.sampleRate / n;
      const w = this.localBinWindow;

      const targetMag = this.dsp.goertzel(windowed, freq);

      let sum = 0;
      let count = 0;

      for (let i = 1; i <= w; i++) {
        const f1 = freq - i * resolution;
        const f2 = freq + i * resolution;

        if (f1 > 0 && !this.isNearOtherCarrier(f1, freq)) {
          sum += this.dsp.goertzel(windowed, f1);
          count++;
        }

        if (f2 < this.sampleRate / 2 && !this.isNearOtherCarrier(f2, freq)) {
          sum += this.dsp.goertzel(windowed, f2);
          count++;
        }
      }

      if (count === 0) {
        return targetMag > this.detectionThreshold ? 1 : 0;
      }

      const localMean = sum / count;
      return targetMag / (localMean + eps);
    }

    let desired = n * this.fftPadFactor;
    let fftSize = 1;
    while (fftSize < desired) fftSize <<= 1;

    const padded = new Float32Array(fftSize);
    for (let i = 0; i < n; i++) {
      padded[i] = windowed[i];
    }

    const { magnitude } = this.dsp.fft(padded);

    const bin = Math.round((freq * fftSize) / this.sampleRate);
    if (bin < 0 || bin >= magnitude.length) return 0;

    const w = this.localBinWindow;

    let sum = 0;
    let count = 0;

    const start = Math.max(0, bin - w);
    const end = Math.min(magnitude.length - 1, bin + w);

    for (let i = start; i <= end; i++) {
      if (i === bin || this.isOtherCarrierBin(i, bin, fftSize)) continue;
      sum += magnitude[i];
      count++;
    }

    if (count === 0) {
      return magnitude[bin] > this.detectionThreshold ? 1 : 0;
    }

    const localMean = sum / count;
    return magnitude[bin] / (localMean + eps);
  }

  private isNearOtherCarrier(freq: number, targetFreq: number): boolean {
    const guardHz = Math.max(
      this.sampleRate / this.frameSize,
      this.frequencyStep * 0.2,
    );
    for (let bitIndex = 0; bitIndex < this.bitsPerSymbol; bitIndex++) {
      const carrier = this.frequencyForBit(bitIndex);
      if (Math.abs(carrier - targetFreq) < 1) continue;
      if (Math.abs(freq - carrier) <= guardHz) {
        return true;
      }
    }
    return false;
  }
}
