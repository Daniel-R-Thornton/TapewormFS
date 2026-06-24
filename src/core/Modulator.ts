import { DSPEngine } from "./DSPEngine";

export interface IQSymbol {
  i: number;
  q: number;
}

export class Modulator {
  private dsp: DSPEngine;
  public lastIQSymbols: IQSymbol[] = [];

  constructor(dsp: DSPEngine) {
    this.dsp = dsp;
  }

  // ============ BPSK (Binary Phase Shift Keying) ============
  bpskModulate(
    bits: number[],
    carrierFreq: number,
    symbolRate: number,
    duration: number,
  ): Float32Array {
    const samplesPerSymbol = this.dsp.sampleRate / symbolRate;
    const totalSamples = Math.floor(duration * this.dsp.sampleRate);
    const output = new Float32Array(totalSamples);

    this.lastIQSymbols = [];

    for (
      let i = 0;
      i < bits.length && i * samplesPerSymbol < totalSamples;
      i++
    ) {
      const phase = bits[i] === 1 ? 0 : Math.PI;
      const start = Math.floor(i * samplesPerSymbol);
      const end = Math.min(start + samplesPerSymbol, totalSamples);

      // Store IQ representation
      this.lastIQSymbols.push({ i: bits[i] === 1 ? 1 : -1, q: 0 });

      for (let s = start; s < end; s++) {
        const t = s / this.dsp.sampleRate;
        output[s] = Math.sin(2 * Math.PI * carrierFreq * t + phase);
      }
    }

    return output;
  }

  // ============ QPSK (Quadrature Phase Shift Keying) ============
  qpskModulate(
    symbols: number[],
    carrierFreq: number,
    symbolRate: number,
    duration: number,
  ): Float32Array {
    const samplesPerSymbol = this.dsp.sampleRate / symbolRate;
    const totalSamples = Math.floor(duration * this.dsp.sampleRate);
    const output = new Float32Array(totalSamples);

    this.lastIQSymbols = [];
    const phases = [
      Math.PI / 4,
      (3 * Math.PI) / 4,
      (5 * Math.PI) / 4,
      (7 * Math.PI) / 4,
    ];

    for (
      let i = 0;
      i < symbols.length && i * samplesPerSymbol < totalSamples;
      i++
    ) {
      const phase = phases[symbols[i] % 4];
      const start = Math.floor(i * samplesPerSymbol);
      const end = Math.min(start + samplesPerSymbol, totalSamples);

      this.lastIQSymbols.push({ i: Math.cos(phase), q: Math.sin(phase) });

      for (let s = start; s < end; s++) {
        const t = s / this.dsp.sampleRate;
        output[s] = Math.sin(2 * Math.PI * carrierFreq * t + phase);
      }
    }

    return output;
  }

  // ============ OFDM (Orthogonal Frequency Division Multiplexing) ============
  // You implement this! Use IFFT to create multiple orthogonal carriers
  ofdmModulate(_symbols: number[][], _carrierSpacing: number): Float32Array {
    // TODO: Implement OFDM using IFFT
    // symbols: 2D array [carrierIndex][symbolIndex]
    // Each carrier gets its own symbol sequence
    throw new Error("OFDM modulation not yet implemented - your turn!");
  }

  // ============ PSK with Raised Cosine Filtering (Pulse Shaping) ============
  raisedCosineFilter(rolloff: number, span: number): Float32Array {
    const filterLen = span * this.dsp.sampleRate;
    const filter = new Float32Array(filterLen);
    const center = filterLen / 2;

    for (let i = 0; i < filterLen; i++) {
      const t = (i - center) / this.dsp.sampleRate;
      if (t === 0) filter[i] = 1;
      else if (Math.abs(t) === 1 / (2 * rolloff)) {
        filter[i] = (Math.PI / 4) * Math.sin((Math.PI * t) / rolloff);
      } else {
        filter[i] =
          ((Math.sin(Math.PI * t) / (Math.PI * t)) *
            Math.cos(Math.PI * rolloff * t)) /
          (1 - (2 * rolloff * t) ** 2);
      }
    }
    return filter;
  }

  // ============ Additive White Gaussian Noise (for testing) ============
  addAWGN(signal: Float32Array, snr_dB: number): Float32Array {
    const signalPower =
      signal.reduce((sum, s) => sum + s * s, 0) / signal.length;
    const noisePower = signalPower / Math.pow(10, snr_dB / 10);
    const noiseStd = Math.sqrt(noisePower);

    const noisy = new Float32Array(signal.length);
    for (let i = 0; i < signal.length; i++) {
      const noise = this.gaussianRandom(0, noiseStd);
      noisy[i] = signal[i] + noise;
    }
    return noisy;
  }

  private gaussianRandom(mean: number, std: number): number {
    let u = 0,
      v = 0;
    while (u === 0) u = Math.random();
    while (v === 0) v = Math.random();
    const z = Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
    return mean + z * std;
  }
}
