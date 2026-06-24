import { DSPEngine } from "../core/DSPEngine";
import { IQSymbol } from "../core/Modulator";

export interface Viewport {
  startSample: number;
  endSample: number;
}

export class Visualizer {
  drawWaveform(
    canvas: HTMLCanvasElement,
    samples: Float32Array,
    color = "#4a9eff",
    viewport?: Viewport,
  ) {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const displayWidth = Math.max(1, Math.floor(rect.width));
    const displayHeight = Math.max(1, Math.floor(rect.height));
    const actualWidth = Math.max(1, Math.floor(displayWidth * dpr));
    const actualHeight = Math.max(1, Math.floor(displayHeight * dpr));

    if (canvas.width !== actualWidth || canvas.height !== actualHeight) {
      canvas.width = actualWidth;
      canvas.height = actualHeight;
    }

    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const w = displayWidth;
    const h = displayHeight;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, w, h);

    if (!samples || samples.length === 0) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText("No signal loaded", 20, h / 2);
      return;
    }

    // Use viewport or default to full range
    const start = viewport?.startSample ?? 0;
    const end = viewport?.endSample ?? samples.length;
    const visibleSamples = end - start;

    if (visibleSamples <= 0) return;

    const samplesPerPixel = visibleSamples / w;
    const midY = h / 2;
    const amplitude = h / 2 - 10;

    // Draw grid
    ctx.strokeStyle = "#1a1a2a";
    ctx.lineWidth = 0.5;
    for (let y = 0; y <= 4; y++) {
      const yPos = midY + (y - 2) * (h / 4);
      ctx.beginPath();
      ctx.moveTo(0, yPos);
      ctx.lineTo(w, yPos);
      ctx.stroke();
    }

    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";

    if (visibleSamples <= Math.max(w * 2, 10000)) {
      ctx.beginPath();
      let firstPoint = true;
      for (let i = 0; i < visibleSamples; i++) {
        const value = samples[start + i];
        const x = (i / visibleSamples) * w;
        const y = midY + value * amplitude;
        if (firstPoint) {
          ctx.moveTo(x, y);
          firstPoint = false;
        } else {
          ctx.lineTo(x, y);
        }
      }
      ctx.stroke();
    } else {
      ctx.beginPath();
      let hasData = false;
      for (let x = 0; x < w; x++) {
        const colStart = start + Math.floor(x * samplesPerPixel);
        const colEnd = Math.min(
          end,
          start + Math.floor((x + 1) * samplesPerPixel),
        );
        if (colStart >= end) break;

        let minVal = Infinity;
        let maxVal = -Infinity;
        let sumVal = 0;
        let count = 0;
        for (let i = colStart; i < colEnd; i++) {
          const value = samples[i];
          if (value < minVal) minVal = value;
          if (value > maxVal) maxVal = value;
          sumVal += value;
          count += 1;
        }
        if (count === 0) continue;
        const avgVal = sumVal / count;

        const yMin = midY + minVal * amplitude;
        const yMax = midY + maxVal * amplitude;
        const yAvg = midY + avgVal * amplitude;

        ctx.moveTo(x, yMin);
        ctx.lineTo(x, yMax);
        ctx.moveTo(x - 0.5, yAvg);
        ctx.lineTo(x + 0.5, yAvg);
        hasData = true;
      }
      if (hasData) ctx.stroke();
    }

    // Zero line
    ctx.beginPath();
    ctx.strokeStyle = "#ff6b4a66";
    ctx.moveTo(0, midY);
    ctx.lineTo(w, midY);
    ctx.stroke();

    // Draw time labels
    ctx.fillStyle = "#888";
    ctx.font = "10px monospace";
    const duration = samples.length / 48000; // assume 48kHz
    for (let i = 0; i <= 4; i++) {
      const t =
        (start / samples.length) * duration +
        (i / 4) * ((visibleSamples / samples.length) * duration);
      const x = (i / 4) * w;
      ctx.fillText(`${t.toFixed(2)}s`, x + 5, h - 5);
    }
  }

  drawMagnitudeSpectrum(
    canvas: HTMLCanvasElement,
    samples: Float32Array,
    dsp: DSPEngine,
    fftSize = 2048,
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, w, h);

    if (!samples || samples.length === 0) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText("No signal loaded", 20, h / 2);
      return;
    }

    // Apply window to reduce spectral leakage
    const windowed = dsp.applyWindow(
      samples.slice(0, Math.min(samples.length, fftSize)),
      "hann",
    );
    const padded = new Float32Array(fftSize);
    for (let i = 0; i < Math.min(windowed.length, fftSize); i++)
      padded[i] = windowed[i];

    const { magnitude } = dsp.fft(padded);

    // Find max magnitude for normalization (exclude DC)
    let maxMag = 0;
    for (let i = 10; i < magnitude.length; i++) {
      if (magnitude[i] > maxMag) maxMag = magnitude[i];
    }
    if (maxMag === 0) maxMag = 1;

    // Convert to dB (log scale for better visibility)
    const dbData = new Float32Array(magnitude.length);
    for (let i = 0; i < magnitude.length; i++) {
      let db = 20 * Math.log10(magnitude[i] / maxMag + 0.00001);
      db = Math.max(-80, Math.min(0, db)); // Clamp to -80..0 dB
      dbData[i] = (db + 80) / 80; // Normalize to 0..1
    }

    // Draw frequency grid
    ctx.strokeStyle = "#1a1a2a";
    ctx.lineWidth = 0.5;
    const nyquist = dsp.sampleRate / 2;
    for (let i = 0; i <= 8; i++) {
      const freq = (i / 8) * nyquist;
      const x = (freq / nyquist) * w;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();

      ctx.fillStyle = "#666";
      ctx.font = "9px monospace";
      ctx.fillText(`${Math.round(freq)}Hz`, x + 2, h - 5);
    }

    // Draw magnitude spectrum
    ctx.beginPath();
    ctx.strokeStyle = "#ff6b4a";
    ctx.lineWidth = 1.5;

    for (let x = 0; x < w; x++) {
      const bin = Math.floor((x / w) * dbData.length);
      if (bin < dbData.length) {
        const norm = dbData[bin];
        const y = h - norm * (h - 40);
        if (x === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
    }
    ctx.stroke();

    // Fill area under curve
    ctx.beginPath();
    for (let x = 0; x < w; x++) {
      const bin = Math.floor((x / w) * dbData.length);
      if (bin < dbData.length) {
        const norm = dbData[bin];
        const y = h - norm * (h - 40);
        if (x === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
    }
    ctx.lineTo(w, h);
    ctx.lineTo(0, h);
    ctx.fillStyle = "#ff6b4a20";
    ctx.fill();

    // Draw peak markers
    const peaks = dsp.findPeaks(magnitude, dsp.sampleRate, fftSize);
    ctx.fillStyle = "#4a9eff";
    ctx.font = "10px monospace";
    for (const peak of peaks.slice(0, 5)) {
      const x = (peak.freq / nyquist) * w;
      const y =
        h -
        ((20 * Math.log10(peak.magnitude / maxMag + 0.00001) + 80) / 80) *
          (h - 40);
      ctx.beginPath();
      ctx.arc(x, y, 4, 0, Math.PI * 2);
      ctx.fillStyle = "#4a9eff";
      ctx.fill();
      ctx.fillStyle = "#fff";
      ctx.fillText(`${Math.round(peak.freq)}Hz`, x + 5, y - 5);
    }

    // Labels
    ctx.fillStyle = "#aaa";
    ctx.font = "11px monospace";
    ctx.fillText("Frequency (Hz) →", w - 120, h - 10);
    ctx.save();
    ctx.translate(15, h / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText("Magnitude (dB) ↑", -40, 0);
    ctx.restore();
  }

  drawSpectrogram(
    canvas: HTMLCanvasElement,
    samples: Float32Array,
    dsp: DSPEngine,
    options?: {
      fftSize?: number;
      colormap?: "jet" | "hot" | "gray";
      minDb?: number;
      maxDb?: number;
    },
    viewport?: Viewport,
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;
    const fftSize = options?.fftSize ?? 1024;
    const hopSize = fftSize / 4;
    const colormap = options?.colormap ?? "jet";
    const minDb = options?.minDb ?? -80;
    const maxDb = options?.maxDb ?? -20;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, w, h);

    const startSample = viewport?.startSample ?? 0;
    const endSample = viewport?.endSample ?? samples.length;
    const visible = samples.subarray(startSample, endSample);

    if (!visible || visible.length < fftSize) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText(
        `Need at least ${fftSize} samples for spectrogram`,
        20,
        h / 2,
      );
      return;
    }

    const specData = dsp.spectrogram(visible, fftSize, hopSize);
    if (specData.length === 0) return;

    const nyquist = dsp.sampleRate / 2;

    // Draw spectrogram
    for (let x = 0; x < w && x < specData.length; x++) {
      const frame = specData[x];
      for (let y = 0; y < h; y++) {
        const bin = Math.floor((y / h) * frame.length);
        if (bin < frame.length) {
          // Convert to dB
          let db = 20 * Math.log10(frame[bin] + 0.00001);
          db = Math.max(minDb, Math.min(maxDb, db));
          const norm = (db - minDb) / (maxDb - minDb);

          const color = this.colormapValue(norm, colormap);
          ctx.fillStyle = color;
          ctx.fillRect(x, h - y, 1, 1);
        }
      }
    }

    // Draw frequency labels
    ctx.fillStyle = "#aaa";
    ctx.font = "9px monospace";
    for (let i = 0; i <= 4; i++) {
      const freq = (i / 4) * nyquist;
      const y = h - (i / 4) * h;
      ctx.fillText(`${Math.round(freq)}Hz`, 5, y - 2);
    }

    // Draw time labels
    const duration = samples.length / dsp.sampleRate;
    for (let i = 0; i <= 4; i++) {
      const t = (i / 4) * duration;
      const x = (i / 4) * Math.min(w, specData.length);
      ctx.fillStyle = "#aaa";
      ctx.fillText(`${t.toFixed(1)}s`, x + 5, h - 5);
    }

    // Colorbar
    this.drawColorbar(ctx, w - 30, 20, 20, h - 40, minDb, maxDb, colormap);
  }

  private colormapValue(norm: number, type: string): string {
    norm = Math.max(0, Math.min(1, norm));

    if (type === "hot") {
      const r = Math.min(255, Math.floor(255 * norm * 1.5));
      const g = Math.min(255, Math.floor(255 * norm * 0.8));
      const b = Math.min(255, Math.floor(255 * norm * 0.3));
      return `rgb(${r}, ${g}, ${b})`;
    } else if (type === "gray") {
      const v = Math.floor(255 * norm);
      return `rgb(${v}, ${v}, ${v})`;
    } else {
      // jet colormap
      const r = Math.min(255, Math.floor(255 * (1.5 - Math.abs(4 * norm - 3))));
      const g = Math.min(255, Math.floor(255 * (1.5 - Math.abs(4 * norm - 2))));
      const b = Math.min(255, Math.floor(255 * (1.5 - Math.abs(4 * norm - 1))));
      return `rgb(${Math.max(0, r)}, ${Math.max(0, g)}, ${Math.max(0, b)})`;
    }
  }
  // New method: Split a signal into frequency bands and display each separately
  drawSplitCarriers(
    canvas: HTMLCanvasElement,
    samples: Float32Array,
    dsp: DSPEngine,
    carrierFrequencies: number[],
    bandwidth: number = 100,
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, w, h);

    if (!samples || samples.length === 0 || carrierFrequencies.length === 0) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText("No carriers to display", 20, h / 2);
      return;
    }

    const bandHeight = (h - 40) / carrierFrequencies.length;
    const colors = [
      "#4a9eff",
      "#ff6b4a",
      "#5eead4",
      "#f472b6",
      "#a78bfa",
      "#fbbf24",
      "#34d399",
      "#f97316",
    ];

    // Isolate each carrier using bandpass filtering
    for (let idx = 0; idx < carrierFrequencies.length; idx++) {
      const freq = carrierFrequencies[idx];
      const yOffset = 20 + idx * bandHeight;
      const color = colors[idx % colors.length];

      // Draw band label
      ctx.fillStyle = color;
      ctx.font = "10px monospace";
      ctx.fillText(
        `Carrier ${idx}: ${freq}Hz ±${bandwidth / 2}Hz`,
        10,
        yOffset + 12,
      );

      // Extract this frequency band
      const bandSignal = this.bandpassFilter(
        samples,
        freq,
        bandwidth,
        dsp.sampleRate,
      );

      // Draw the isolated signal
      this.drawSignalInBand(
        ctx,
        bandSignal,
        w,
        bandHeight - 15,
        yOffset + 18,
        color,
      );
    }

    // Draw time axis
    ctx.fillStyle = "#666";
    ctx.font = "9px monospace";
    const duration = samples.length / dsp.sampleRate;
    for (let i = 0; i <= 4; i++) {
      const t = (i / 4) * duration;
      const x = (i / 4) * w;
      ctx.fillText(`${t.toFixed(2)}s`, x + 5, h - 5);
    }
  }

  // Draw a single signal in a band
  private drawSignalInBand(
    ctx: CanvasRenderingContext2D,
    signal: Float32Array,
    width: number,
    height: number,
    yOffset: number,
    color: string,
  ) {
    if (!signal || signal.length === 0) return;

    const step = Math.max(1, Math.floor(signal.length / width));
    const midY = yOffset + height / 2;

    // Draw envelope bounds
    let envelopeMax = 0;
    for (let i = 0; i < signal.length; i++) {
      const absVal = Math.abs(signal[i]);
      if (absVal > envelopeMax) envelopeMax = absVal;
    }
    if (envelopeMax === 0) envelopeMax = 1;

    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.2;

    for (let x = 0; x < width; x++) {
      const sampleIdx = Math.floor(x * step);
      if (sampleIdx >= signal.length) break;
      const y = midY + (signal[sampleIdx] / envelopeMax) * (height / 2 - 2);
      if (x === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Draw zero line
    ctx.beginPath();
    ctx.strokeStyle = `${color}66`;
    ctx.moveTo(0, midY);
    ctx.lineTo(width, midY);
    ctx.stroke();
  }

  // Simple bandpass filter using FFT (quick and dirty for visualization)
  private bandpassFilter(
    samples: Float32Array,
    centerFreq: number,
    bandwidth: number,
    sampleRate: number,
  ): Float32Array {
    const fftSize = 2048;
    const padded = new Float32Array(fftSize);
    for (let i = 0; i < Math.min(samples.length, fftSize); i++)
      padded[i] = samples[i];

    const { real, imag } = this.complexFFT(padded);

    const nyquist = sampleRate / 2;
    const lowFreq = Math.max(0, centerFreq - bandwidth / 2);
    const highFreq = Math.min(nyquist, centerFreq + bandwidth / 2);

    // Zero out frequencies outside the band
    for (let i = 0; i < real.length; i++) {
      const freq = (i * sampleRate) / fftSize;
      if (freq < lowFreq || freq > highFreq) {
        real[i] = 0;
        imag[i] = 0;
      }
    }

    return this.complexIFFT(real, imag);
  }

  private complexFFT(samples: Float32Array): {
    magnitude: Float32Array;
    phase: Float32Array;
    real: Float64Array;
    imag: Float64Array;
  } {
    const n = samples.length;
    const real = new Float64Array(n);
    const imag = new Float64Array(n);
    for (let i = 0; i < n; i++) real[i] = samples[i];

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

  private complexIFFT(real: Float64Array, imag: Float64Array): Float32Array {
    const n = real.length;
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

  // Draw energy per carrier (bar chart showing how much power in each frequency)
  drawCarrierEnergy(
    canvas: HTMLCanvasElement,
    samples: Float32Array,
    dsp: DSPEngine,
    carrierFrequencies: number[],
    bandwidth: number = 100,
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, w, h);

    if (!samples || samples.length === 0 || carrierFrequencies.length === 0) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText("No carriers to analyze", 20, h / 2);
      return;
    }

    const barWidth = (w - 100) / carrierFrequencies.length;
    const colors = [
      "#4a9eff",
      "#ff6b4a",
      "#5eead4",
      "#f472b6",
      "#a78bfa",
      "#fbbf24",
    ];

    // Calculate energy in each carrier band
    const energies: number[] = [];
    let maxEnergy = 0;

    for (let idx = 0; idx < carrierFrequencies.length; idx++) {
      const freq = carrierFrequencies[idx];
      const bandSignal = this.bandpassFilter(
        samples,
        freq,
        bandwidth,
        dsp.sampleRate,
      );
      let energy = 0;
      for (let i = 0; i < bandSignal.length; i++) {
        energy += bandSignal[i] * bandSignal[i];
      }
      energy = Math.sqrt(energy / bandSignal.length); // RMS
      energies.push(energy);
      if (energy > maxEnergy) maxEnergy = energy;
    }

    if (maxEnergy === 0) maxEnergy = 1;

    // Draw bars
    for (let idx = 0; idx < carrierFrequencies.length; idx++) {
      const x = 50 + idx * barWidth;
      const energyNorm = energies[idx] / maxEnergy;
      const barHeight = energyNorm * (h - 60);
      const color = colors[idx % colors.length];

      // Bar
      ctx.fillStyle = color;
      ctx.fillRect(x, h - 20 - barHeight, barWidth - 4, barHeight);

      // Energy value
      ctx.fillStyle = "#aaa";
      ctx.font = "9px monospace";
      const db = 20 * Math.log10(energies[idx] + 0.00001);
      ctx.fillText(`${db.toFixed(1)}dB`, x + 2, h - 25 - barHeight);

      // Frequency label
      ctx.fillStyle = color;
      ctx.font = "10px monospace";
      ctx.fillText(`${carrierFrequencies[idx]}Hz`, x + 2, h - 8);
    }

    ctx.fillStyle = "#aaa";
    ctx.font = "11px monospace";
    ctx.fillText("Carrier Frequency →", w / 2 - 80, h - 5);
    ctx.save();
    ctx.translate(15, h / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText("Energy (RMS) ↑", -40, 0);
    ctx.restore();
  }
  private drawColorbar(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    w: number,
    h: number,
    minDb: number,
    maxDb: number,
    colormap: string,
  ) {
    for (let i = 0; i < h; i++) {
      const norm = i / h;
      ctx.fillStyle = this.colormapValue(1 - norm, colormap);
      ctx.fillRect(x, y + i, w, 1);
    }

    ctx.fillStyle = "#aaa";
    ctx.font = "8px monospace";
    ctx.save();
    ctx.translate(x + w + 5, y);
    ctx.fillText(`${maxDb}dB`, 0, 8);
    ctx.fillText(`${minDb}dB`, 0, h);
    ctx.restore();
  }

  drawConstellation(canvas: HTMLCanvasElement, symbols: IQSymbol[]) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const size = canvas.width;
    const center = size / 2;

    ctx.fillStyle = "#050508";
    ctx.fillRect(0, 0, size, size);

    if (!symbols || symbols.length === 0) {
      ctx.fillStyle = "#888";
      ctx.font = "12px monospace";
      ctx.fillText("No IQ data", center - 40, center);
      return;
    }

    // Grid
    ctx.strokeStyle = "#2a2a3a";
    ctx.lineWidth = 1;
    for (let i = -1; i <= 1; i++) {
      const x = center + i * (size / 3);
      const y = center + i * (size / 3);
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, size);
      ctx.moveTo(0, y);
      ctx.lineTo(size, y);
      ctx.stroke();
    }

    // Draw points with intensity based on frequency
    const counts = new Map<string, number>();
    for (const sym of symbols) {
      const key = `${sym.i.toFixed(2)},${sym.q.toFixed(2)}`;
      counts.set(key, (counts.get(key) || 0) + 1);
    }

    const maxCount = Math.max(...counts.values());

    for (const [key, count] of counts) {
      const [iStr, qStr] = key.split(",");
      const i = parseFloat(iStr);
      const q = parseFloat(qStr);
      const x = center + i * (size / 2 - 20);
      const y = center - q * (size / 2 - 20);
      const intensity = Math.min(1, count / maxCount);

      ctx.fillStyle = `rgba(74, 158, 255, ${0.3 + intensity * 0.7})`;
      ctx.beginPath();
      ctx.arc(x, y, 3 + intensity * 3, 0, Math.PI * 2);
      ctx.fill();
      ctx.strokeStyle = "#fff";
      ctx.lineWidth = 0.5;
      ctx.stroke();
    }

    // Labels
    ctx.fillStyle = "#888";
    ctx.font = "10px monospace";
    ctx.fillText("In-Phase (I) →", size - 80, center + 20);
    ctx.save();
    ctx.translate(20, center);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText("Quadrature (Q) ↑", -40, 0);
    ctx.restore();
  }
}
