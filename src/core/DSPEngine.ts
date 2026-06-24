import {
  buildWavWithMetadata,
  type ModemWavMetadata,
} from "../io/WavMetadata";

export class DSPEngine {
  sampleRate: number;

  constructor(sampleRate = 48000) {
    this.sampleRate = sampleRate;
  }

  // ============ FFT / IFFT ============
  fft(samples: Float32Array): {
    magnitude: Float32Array;
    phase: Float32Array;
    real: Float64Array;
    imag: Float64Array;
  } {
    const n = samples.length;
    if ((n & (n - 1)) !== 0) throw new Error("FFT length must be power of 2");

    const real = new Float64Array(n);
    const imag = new Float64Array(n);
    for (let i = 0; i < n; i++) real[i] = samples[i];

    // Bit reversal
    const bits = Math.log2(n);
    for (let i = 0; i < n; i++) {
      let rev = 0;
      for (let j = 0; j < bits; j++) rev = (rev << 1) | ((i >> j) & 1);
      if (rev > i) {
        [real[i], real[rev]] = [real[rev], real[i]];
        [imag[i], imag[rev]] = [imag[rev], imag[i]];
      }
    }

    // FFT
    for (let len = 2; len <= n; len <<= 1) {
      const angle = (-2 * Math.PI) / len;
      const wlenReal = Math.cos(angle);
      const wlenImag = Math.sin(angle);

      for (let i = 0; i < n; i += len) {
        let wReal = 1,
          wImag = 0;
        for (let j = 0; j < len / 2; j++) {
          const uReal = real[i + j];
          const uImag = imag[i + j];
          const vReal =
            real[i + j + len / 2] * wReal - imag[i + j + len / 2] * wImag;
          const vImag =
            real[i + j + len / 2] * wImag + imag[i + j + len / 2] * wReal;

          real[i + j] = uReal + vReal;
          imag[i + j] = uImag + vImag;
          real[i + j + len / 2] = uReal - vReal;
          imag[i + j + len / 2] = uImag - vImag;

          const nextWReal = wReal * wlenReal - wImag * wlenImag;
          const nextWImag = wReal * wlenImag + wImag * wlenReal;
          wReal = nextWReal;
          wImag = nextWImag;
        }
      }
    }

    const magnitude = new Float32Array(n / 2);
    const phase = new Float32Array(n / 2);
    for (let i = 0; i < n / 2; i++) {
      magnitude[i] = Math.hypot(real[i], imag[i]);
      phase[i] = Math.atan2(imag[i], real[i]);
    }

    return { magnitude, phase, real, imag };
  }

  ifft(magnitude: Float32Array, phase: Float32Array): Float32Array {
    const n = magnitude.length * 2;
    const real = new Float64Array(n);
    const imag = new Float64Array(n);

    for (let i = 0; i < n / 2; i++) {
      real[i] = magnitude[i] * Math.cos(phase[i]);
      imag[i] = magnitude[i] * Math.sin(phase[i]);
      if (i > 0) {
        real[n - i] = real[i];
        imag[n - i] = -imag[i];
      }
    }

    const bits = Math.log2(n);
    for (let i = 0; i < n; i++) {
      let rev = 0;
      for (let j = 0; j < bits; j++) rev = (rev << 1) | ((i >> j) & 1);
      if (rev > i) {
        [real[i], real[rev]] = [real[rev], real[i]];
        [imag[i], imag[rev]] = [imag[rev], imag[i]];
      }
    }

    for (let len = 2; len <= n; len <<= 1) {
      const angle = (2 * Math.PI) / len;
      const wlenReal = Math.cos(angle);
      const wlenImag = Math.sin(angle);

      for (let i = 0; i < n; i += len) {
        let wReal = 1,
          wImag = 0;
        for (let j = 0; j < len / 2; j++) {
          const uReal = real[i + j];
          const uImag = imag[i + j];
          const vReal =
            real[i + j + len / 2] * wReal - imag[i + j + len / 2] * wImag;
          const vImag =
            real[i + j + len / 2] * wImag + imag[i + j + len / 2] * wReal;

          real[i + j] = uReal + vReal;
          imag[i + j] = uImag + vImag;
          real[i + j + len / 2] = uReal - vReal;
          imag[i + j + len / 2] = uImag - vImag;

          const nextWReal = wReal * wlenReal - wImag * wlenImag;
          const nextWImag = wReal * wlenImag + wImag * wlenReal;
          wReal = nextWReal;
          wImag = nextWImag;
        }
      }
    }

    const samples = new Float32Array(n);
    for (let i = 0; i < n; i++) samples[i] = real[i] / n;
    return samples;
  }

  // ============ Signal Generation ============
  generateCarrier(
    frequency: number,
    duration: number,
    amplitude = 1,
  ): Float32Array {
    const totalSamples = Math.floor(duration * this.sampleRate);
    const samples = new Float32Array(totalSamples);
    for (let i = 0; i < totalSamples; i++) {
      const t = i / this.sampleRate;
      samples[i] = amplitude * Math.sin(2 * Math.PI * frequency * t);
    }
    return samples;
  }

  generateChirp(
    startFreq: number,
    endFreq: number,
    duration: number,
  ): Float32Array {
    const totalSamples = Math.floor(duration * this.sampleRate);
    const samples = new Float32Array(totalSamples);
    for (let i = 0; i < totalSamples; i++) {
      const t = i / this.sampleRate;
      const f = startFreq + (endFreq - startFreq) * (t / duration);
      samples[i] = Math.sin(2 * Math.PI * f * t);
    }
    return samples;
  }

  // ============ Signal Processing ============
  mix(signals: Float32Array[]): Float32Array {
    if (signals.length === 0) return new Float32Array(0);
    const length = Math.max(...signals.map((s) => s.length));
    const mixed = new Float32Array(length);
    for (const signal of signals) {
      for (let i = 0; i < signal.length; i++) mixed[i] += signal[i];
    }
    let max = 0;
    for (let i = 0; i < mixed.length; i++)
      if (Math.abs(mixed[i]) > max) max = Math.abs(mixed[i]);
    if (max > 0) for (let i = 0; i < mixed.length; i++) mixed[i] /= max;
    return mixed;
  }

  applyWindow(
    samples: Float32Array,
    type: "hann" | "hamming" = "hann",
  ): Float32Array {
    const n = samples.length;
    const windowed = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      let w = 0;
      if (type === "hann")
        w = 0.5 * (1 - Math.cos((2 * Math.PI * i) / (n - 1)));
      else if (type === "hamming")
        w = 0.54 - 0.46 * Math.cos((2 * Math.PI * i) / (n - 1));
      windowed[i] = samples[i] * w;
    }
    return windowed;
  }

  spectrogram(
    samples: Float32Array,
    fftSize = 1024,
    hopSize = 512,
  ): number[][] {
    const result: number[][] = [];
    const window = new Float32Array(fftSize);
    window.fill(1);
    const windowed = this.applyWindow(window);

    for (let start = 0; start + fftSize <= samples.length; start += hopSize) {
      const frame = new Float32Array(fftSize);
      for (let i = 0; i < fftSize; i++)
        frame[i] = samples[start + i] * windowed[i];
      const { magnitude } = this.fft(frame);
      result.push(Array.from(magnitude));
    }
    return result;
  }

  // ============ Analysis ============
  analyzeFrequency(
    samples: Float32Array,
    fftSize: number,
  ): { peaks: { freq: number; magnitude: number }[]; noiseFloor: number } {
    const padded = new Float32Array(fftSize);
    for (let i = 0; i < Math.min(samples.length, fftSize); i++)
      padded[i] = samples[i];

    const { magnitude } = this.fft(padded);
    const peaks: { freq: number; magnitude: number }[] = [];

    for (let i = 1; i < magnitude.length - 1; i++) {
      if (
        magnitude[i] > magnitude[i - 1] &&
        magnitude[i] > magnitude[i + 1] &&
        magnitude[i] > 0.01
      ) {
        const freq = (i * this.sampleRate) / fftSize;
        peaks.push({ freq, magnitude: magnitude[i] });
      }
    }

    let noiseSum = 0;
    for (let i = 0; i < Math.min(20, magnitude.length); i++)
      noiseSum += magnitude[i];
    const noiseFloor = noiseSum / 20;

    return { peaks, noiseFloor };
  }

  // ============ WAV Conversion ============
  floatToInt16(samples: Float32Array): Int16Array {
    const ints = new Int16Array(samples.length);
    for (let i = 0; i < samples.length; i++) {
      let sample = Math.max(-1, Math.min(1, samples[i]));
      ints[i] = Math.floor(sample * 32767);
    }
    return ints;
  }

  int16ToFloat(ints: Int16Array): Float32Array {
    const samples = new Float32Array(ints.length);
    for (let i = 0; i < ints.length; i++) samples[i] = ints[i] / 32768;
    return samples;
  }

  samplesToWav(samples: Float32Array, metadata?: ModemWavMetadata): Blob {
    const ints = this.floatToInt16(samples);
    if (metadata) {
      return buildWavWithMetadata(ints, this.sampleRate, metadata);
    }

    const buffer = new ArrayBuffer(44 + ints.length * 2);
    const view = new DataView(buffer);

    // WAV header
    const writeString = (offset: number, str: string) => {
      for (let i = 0; i < str.length; i++)
        view.setUint8(offset + i, str.charCodeAt(i));
    };

    writeString(0, "RIFF");
    view.setUint32(4, 36 + ints.length * 2, true);
    writeString(8, "WAVE");
    writeString(12, "fmt ");
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, this.sampleRate, true);
    view.setUint32(28, this.sampleRate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    writeString(36, "data");
    view.setUint32(40, ints.length * 2, true);

    let offset = 44;
    for (let i = 0; i < ints.length; i++) {
      view.setInt16(offset, ints[i], true);
      offset += 2;
    }

    return new Blob([buffer], { type: "audio/wav" });
  }
  findPeaks(
    magnitude: Float32Array,
    sampleRate: number,
    fftSize: number,
  ): { freq: number; magnitude: number }[] {
    const peaks: { freq: number; magnitude: number }[] = [];
    const threshold = 0.05;

    for (let i = 2; i < magnitude.length - 2; i++) {
      if (
        magnitude[i] > magnitude[i - 1] &&
        magnitude[i] > magnitude[i + 1] &&
        magnitude[i] > magnitude[i - 2] &&
        magnitude[i] > magnitude[i + 2] &&
        magnitude[i] > threshold
      ) {
        const freq = (i * sampleRate) / fftSize;
        peaks.push({ freq, magnitude: magnitude[i] });
      }
    }

    return peaks.sort((a, b) => b.magnitude - a.magnitude);
  }

  // Goertzel algorithm: returns magnitude for target frequency in samples
  goertzel(samples: Float32Array, targetFreq: number): number {
    const n = samples.length;
    if (n === 0) return 0;
    const k = Math.round((targetFreq * n) / this.sampleRate);
    const omega = (2 * Math.PI * k) / n;
    const coeff = 2 * Math.cos(omega);
    let s0 = 0,
      s1 = 0,
      s2 = 0;
    for (let i = 0; i < n; i++) {
      s0 = samples[i] + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const real = s1 - s2 * Math.cos(omega);
    const imag = s2 * Math.sin(omega);
    return Math.hypot(real, imag);
  }
}
