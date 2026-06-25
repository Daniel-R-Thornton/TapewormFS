import { DSPEngine } from "./core/DSPEngine";
import { Modulator } from "./core/Modulator";
import { Visualizer } from "./visualization/Visualizer";
import { AudioIO } from "./io/AudioIO";
import { BasicEncoder, BasicDecoder } from "./core/processors/Basic";
import {
  FrequencyPulseEncoder,
  FrequencyPulseDecoder,
} from "./core/processors/FrequencyPulse";
import {
  type ModemWavMetadata,
  scoreDecodedBytes,
} from "./io/WavMetadata";

type AppMode = "lab" | "encode" | "decode";
type PreviewAction =
  | "idle"
  | "generate"
  | "import"
  | "record"
  | "encode-text"
  | "encode-file"
  | "decode-wav"
  | "verify";

interface PreviewState {
  action: PreviewAction;
  title: string;
  badge: string;
  stats: { label: string; value: string }[];
  content: string;
  hasData: boolean;
}

let currentCarrierFrequencies: number[] = [500, 1500, 2500, 3500, 4500, 5500];
const carrierBandwidth = 150;

const dsp = new DSPEngine(48000);
const modulator = new Modulator(dsp);
const visualizer = new Visualizer();
const audioIO = new AudioIO(dsp);

let currentSamples: Float32Array | null = null;
let currentViewport = { startSample: 0, endSample: 0 };
let isPanning = false;
let panStartX = 0;
let panStartViewport: { startSample: number; endSample: number } | null = null;
let spectrogramFftSize = 1024;
let spectrogramColormap: "jet" | "hot" | "gray" = "jet";

let currentMode: AppMode = "lab";
let lastPreviewAction: PreviewAction = "idle";
let lastDecodedBytes: Uint8Array | null = null;
let lastDecodedFileName = "decoded.bin";
let lastEncodedSourceName: string | null = null;
let lastEncodeMetadata: ModemWavMetadata | null = null;
let pendingEncodeFile: File | null = null;
let pendingDecodeFile: File | null = null;

const waveformCanvas = document.getElementById(
  "waveformCanvas",
) as HTMLCanvasElement;
const spectrogramCanvas = document.getElementById(
  "spectrogramCanvas",
) as HTMLCanvasElement;
const constellationCanvas = document.getElementById(
  "constellationCanvas",
) as HTMLCanvasElement;
const spectrumCanvas = document.getElementById(
  "spectrumCanvas",
) as HTMLCanvasElement;
const splitCarriersCanvas = document.getElementById(
  "splitCarriersCanvas",
) as HTMLCanvasElement;
const energyCanvas = document.getElementById(
  "energyCanvas",
) as HTMLCanvasElement;
const debugOutput = document.getElementById(
  "debugOutput",
) as HTMLTextAreaElement;
const decodedOutput = document.getElementById(
  "decodedOutput",
) as HTMLTextAreaElement;
const textInput = document.getElementById("textInput") as HTMLInputElement;
const encoderSelect = document.getElementById(
  "encoderSelect",
) as HTMLSelectElement;
const modemConfigPanel = document.getElementById(
  "modemConfigPanel",
) as HTMLElement;
const pulseFreqInput = document.getElementById(
  "pulseFreqInput",
) as HTMLInputElement;
const frameSizeInput = document.getElementById(
  "frameSizeInput",
) as HTMLInputElement;
const frameInspectorOverviewCanvas = document.getElementById(
  "frameInspectorOverviewCanvas",
) as HTMLCanvasElement | null;
const frameInspectorCanvas = document.getElementById(
  "frameInspectorCanvas",
) as HTMLCanvasElement | null;
const frameScrubber = document.getElementById(
  "fp_frameScrubber",
) as HTMLInputElement | null;
const frameLabel = document.getElementById(
  "fp_frameLabel",
) as HTMLSpanElement | null;
const frameInspectorDetails = document.getElementById(
  "frameInspectorDetails",
) as HTMLDivElement | null;
let inspectorSelectedFrame = 0;
let frameInspectorCount = 0;
const encoderOptionsContainer = document.getElementById(
  "encoderOptionsContainer",
) as HTMLDivElement;
const textFileInput = document.getElementById(
  "textFileInput",
) as HTMLInputElement;
const wavFileInput = document.getElementById(
  "wavFileInput",
) as HTMLInputElement;
const statusBar = document.getElementById("statusBar") as HTMLElement;
const previewTitle = document.getElementById("previewTitle") as HTMLElement;
const previewBadge = document.getElementById("previewBadge") as HTMLElement;
const previewStats = document.getElementById("previewStats") as HTMLElement;
const previewContent = document.getElementById("previewContent") as HTMLElement;
const previewActions = document.getElementById("previewActions") as HTMLElement;
const downloadDecodedBtn = document.getElementById(
  "downloadDecodedBtn",
) as HTMLButtonElement;

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

function formatDuration(samples: number, sampleRate: number): string {
  const sec = samples / sampleRate;
  return sec < 1 ? `${(sec * 1000).toFixed(0)} ms` : `${sec.toFixed(2)} s`;
}

function bytesPreview(bytes: Uint8Array, max = 256): string {
  const slice = bytes.slice(0, max);
  const asText = new TextDecoder().decode(slice);
  const printable = /^[\x20-\x7E\t\n\r]*$/.test(asText);
  if (printable && bytes.length <= max) return asText;
  const hex = Array.from(slice)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join(" ");
  return bytes.length > max ? `${hex}\n… (${bytes.length} bytes total)` : hex;
}

function inferDecodedFilename(wavName: string): string {
  let name = wavName;
  if (name.toLowerCase().endsWith(".wav")) name = name.slice(0, -4);
  if (name.startsWith("encoded_")) name = name.slice("encoded_".length);
  return name || "decoded.bin";
}

function downloadBytes(bytes: Uint8Array, filename: string) {
  const blob = new Blob([bytes.slice()]);
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function downloadCurrentWav(filename: string, metadata?: ModemWavMetadata) {
  if (!currentSamples) {
    log("⚠️ No signal to download.");
    return;
  }
  const blob = dsp.samplesToWav(currentSamples, metadata);
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function renderPreview(state: PreviewState) {
  lastPreviewAction = state.action;
  previewTitle.textContent = state.title;
  previewBadge.textContent = state.badge;
  previewStats.innerHTML = state.stats
    .map(
      (s) =>
        `<div class="stat-card"><div class="stat-label">${s.label}</div><div class="stat-value">${s.value}</div></div>`,
    )
    .join("");
  previewContent.textContent = state.content;
  previewContent.classList.toggle("has-data", state.hasData);
  previewActions.innerHTML = "";
}

function updatePreviewFromSignal(action: PreviewAction, extras?: Partial<PreviewState>) {
  const modeLabels: Record<AppMode, string> = {
    lab: "Signal Lab",
    encode: "Encode",
    decode: "Decode",
  };
  const base: PreviewState = {
    action,
    title: extras?.title ?? "Signal loaded",
    badge: extras?.badge ?? modeLabels[currentMode],
    stats: extras?.stats ?? [],
    content: extras?.content ?? "",
    hasData: extras?.hasData ?? false,
  };

  if (currentSamples) {
    base.stats = [
      {
        label: "Samples",
        value: currentSamples.length.toLocaleString(),
      },
      {
        label: "Duration",
        value: formatDuration(currentSamples.length, dsp.sampleRate),
      },
      {
        label: "Sample rate",
        value: `${dsp.sampleRate.toLocaleString()} Hz`,
      },
      ...(extras?.stats ?? []),
    ];
  }

  renderPreview({ ...base, ...extras, stats: base.stats });
  updateDownloadButtons();
}

function updateDownloadButtons() {
  downloadDecodedBtn.disabled = !lastDecodedBytes?.length;
  const exportDecodedWavBtn = document.getElementById(
    "exportDecodedWavBtn",
  ) as HTMLButtonElement;
  exportDecodedWavBtn.disabled = !currentSamples?.length;
}

function setMode(mode: AppMode) {
  currentMode = mode;
  document.querySelectorAll(".mode-tab").forEach((tab) => {
    tab.classList.toggle(
      "active",
      (tab as HTMLElement).dataset.mode === mode,
    );
  });
  document.querySelectorAll(".control-section").forEach((section) => {
    section.classList.toggle(
      "active",
      (section as HTMLElement).dataset.mode === mode,
    );
  });

  modemConfigPanel.classList.toggle("hidden", mode === "lab");

  refreshPreviewForMode();
}

function refreshPreviewForMode() {
  if (lastPreviewAction !== "idle" && currentSamples) {
    return;
  }

  const placeholders: Record<AppMode, PreviewState> = {
    lab: {
      action: "idle",
      title: "Signal preview",
      badge: "Signal Lab",
      stats: [],
      content:
        "Generate a test signal, import a WAV, or record audio. Waveform and spectrogram update live.",
      hasData: false,
    },
    encode: {
      action: "idle",
      title: "Encode preview",
      badge: "Encode",
      stats: [],
      content:
        "Enter text or drop any file (.bin, .txt, .pdf, etc.) to encode into an audio WAV signal.",
      hasData: false,
    },
    decode: {
      action: "idle",
      title: "Decode preview",
      badge: "Decode",
      stats: [],
      content:
        "Upload a modem WAV to recover the original file. Binary and text files are both supported.",
      hasData: false,
    },
  };
  renderPreview(placeholders[currentMode]);
}

function setupFileDrop(
  dropEl: HTMLElement,
  inputEl: HTMLInputElement,
  nameEl: HTMLElement | null,
  onFile: (file: File) => void,
) {
  const pick = () => inputEl.click();

  dropEl.addEventListener("click", pick);
  dropEl.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      pick();
    }
  });

  dropEl.addEventListener("dragover", (e) => {
    e.preventDefault();
    dropEl.classList.add("dragover");
  });
  dropEl.addEventListener("dragleave", () => {
    dropEl.classList.remove("dragover");
  });
  dropEl.addEventListener("drop", (e) => {
    e.preventDefault();
    dropEl.classList.remove("dragover");
    const file = e.dataTransfer?.files?.[0];
    if (file) {
      if (nameEl) nameEl.textContent = file.name;
      onFile(file);
    }
  });

  inputEl.addEventListener("change", () => {
    const file = inputEl.files?.[0];
    if (file) {
      if (nameEl) nameEl.textContent = file.name;
      onFile(file);
    }
  });
}

function zoomWaveform(factor: number, centerX?: number) {
  if (!currentSamples) return;

  const currentSpan = currentViewport.endSample - currentViewport.startSample;
  let newSpan = currentSpan * factor;
  if (newSpan < 100) newSpan = 100;
  if (newSpan > currentSamples.length) newSpan = currentSamples.length;

  let centerSample =
    (currentViewport.startSample + currentViewport.endSample) / 2;
  if (centerX !== undefined) {
    const rect = waveformCanvas.getBoundingClientRect();
    const relative = Math.max(
      0,
      Math.min(1, (centerX - rect.left) / rect.width),
    );
    centerSample = currentViewport.startSample + relative * currentSpan;
  }

  let newStart = Math.max(0, centerSample - newSpan / 2);
  let newEnd = Math.min(currentSamples.length, newStart + newSpan);
  newStart = Math.max(0, newEnd - newSpan);

  currentViewport = {
    startSample: Math.floor(newStart),
    endSample: Math.floor(newEnd),
  };
  updateWaveformOnly();
}

function resetZoom() {
  if (!currentSamples) return;
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateWaveformOnly();
}

function updateWaveformOnly() {
  if (!currentSamples) return;
  updateAllViews(currentSamples);
}

function setupWaveformPanZoom() {
  const canvases = [waveformCanvas, spectrogramCanvas];
  const addPanListeners = (canvas: HTMLCanvasElement) => {
    canvas.style.cursor = "grab";

    canvas.addEventListener("pointerdown", (e) => {
      if (!currentSamples) return;
      isPanning = true;
      panStartX = e.clientX;
      panStartViewport = { ...currentViewport };
      canvas.setPointerCapture(e.pointerId);
      canvas.style.cursor = "grabbing";
    });

    canvas.addEventListener("pointermove", (e) => {
      if (!isPanning || !currentSamples || !panStartViewport) return;
      const rect = canvas.getBoundingClientRect();
      const width = rect.width || 1;
      const span = panStartViewport.endSample - panStartViewport.startSample;
      const dx = panStartX - e.clientX;
      const deltaSamples = Math.round((dx / width) * span);
      let newStart = panStartViewport.startSample + deltaSamples;
      let newEnd = panStartViewport.endSample + deltaSamples;
      if (newStart < 0) {
        newStart = 0;
        newEnd = span;
      }
      if (newEnd > currentSamples.length) {
        newEnd = currentSamples.length;
        newStart = currentSamples.length - span;
      }
      currentViewport = {
        startSample: Math.floor(newStart),
        endSample: Math.floor(newEnd),
      };
      updateAllViews(currentSamples);
    });

    const endPan = () => {
      if (!isPanning) return;
      isPanning = false;
      panStartViewport = null;
      canvas.style.cursor = "grab";
    };

    canvas.addEventListener("pointerup", endPan);
    canvas.addEventListener("pointercancel", endPan);
    canvas.addEventListener("pointerleave", endPan);

    canvas.addEventListener(
      "wheel",
      (e) => {
        if (!currentSamples) return;
        e.preventDefault();
        const delta = e.deltaY > 0 ? 2 : 0.5;
        zoomWaveform(delta, e.clientX);
      },
      { passive: false },
    );
  };

  canvases.forEach(addPanListeners);
}

function log(message: string, data?: unknown) {
  const timestamp = new Date().toLocaleTimeString();
  const output = `[${timestamp}] ${message}\n${data ? JSON.stringify(data, null, 2) + "\n" : ""}`;
  debugOutput.value = output + debugOutput.value;
  if (debugOutput.value.length > 5000) {
    debugOutput.value = debugOutput.value.slice(0, 5000);
  }
  statusBar.textContent = message;
  console.log(message, data);
}

function activeEncoderType(): string {
  return encoderSelect.value;
}

function buildEncodeMetadata(
  payloadBytes: number,
  sourceName?: string,
): ModemWavMetadata {
  syncEncoderFieldsFromUI();
  const options = getProcessorOptions();
  const flatOptions: Record<string, number | boolean> = {};
  for (const [key, value] of Object.entries(options)) {
    if (typeof value === "number" || typeof value === "boolean") {
      flatOptions[key] = value;
    }
  }
  return {
    v: 1,
    encoder: activeEncoderType() as ModemWavMetadata["encoder"],
    options: flatOptions,
    payloadBytes,
    sourceName,
  };
}

function applyMetadataToUI(metadata: ModemWavMetadata) {
  encoderSelect.value = metadata.encoder;
  renderEncoderOptions();

  const setField = (id: string, value: number | boolean) => {
    const el = document.getElementById(id) as HTMLInputElement | null;
    if (el) el.value = String(value);
  };

  const opts = metadata.options;
  if (metadata.encoder === "basic") {
    if (opts.pulseFrequency != null) {
      pulseFreqInput.value = String(opts.pulseFrequency);
      setField("pulseFreqInput_visible", opts.pulseFrequency);
    }
    if (opts.frameSize != null) {
      frameSizeInput.value = String(opts.frameSize);
      setField("frameSizeInput_visible", opts.frameSize);
    }
  } else {
    if (opts.pulseFrequency != null) setField("fp_baseFreqInput", opts.pulseFrequency);
    if (opts.frequencyStep != null) setField("fp_freqStepInput", opts.frequencyStep);
    if (opts.frameSize != null) setField("fp_frameSizeInput", opts.frameSize);
    if (opts.detectionThreshold != null) {
      setField("fp_detectionThreshold", opts.detectionThreshold);
    }
    if (opts.fftPadFactor != null) setField("fp_fftPadFactor", opts.fftPadFactor);
    if (opts.bitsPerFrame != null) setField("fp_bitsPerFrameInput", opts.bitsPerFrame);
  }

  const goertzel = document.getElementById("fp_useGoertzel") as HTMLInputElement;
  if (goertzel && opts.useGoertzel != null) {
    goertzel.checked = !!opts.useGoertzel;
  }
}

function getProcessorOptions() {
  const parseNumber = (value: string | null | undefined, fallback: number) => {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  };

  if (activeEncoderType() === "frequencyPulse") {
    const base = parseNumber(
      (document.getElementById("fp_baseFreqInput") as HTMLInputElement)?.value,
      parseNumber(pulseFreqInput.value, 10000),
    );
    const step = Math.max(
      1,
      parseNumber(
        (document.getElementById("fp_freqStepInput") as HTMLInputElement)
          ?.value,
        1000,
      ),
    );
    const frame = Math.max(
      1,
      parseNumber(
        (document.getElementById("fp_frameSizeInput") as HTMLInputElement)
          ?.value,
        parseNumber(frameSizeInput.value, 1024),
      ),
    );
    const detectionThreshold = parseNumber(
      (document.getElementById("fp_detectionThreshold") as HTMLInputElement)
        ?.value,
      1.05,
    );
    const fftPadFactor = Math.max(
      1,
      parseNumber(
        (document.getElementById("fp_fftPadFactor") as HTMLInputElement)?.value,
        1,
      ),
    );
    const bitsPerFrame = Math.max(
      1,
      Math.floor(
        parseNumber(
          (document.getElementById("fp_bitsPerFrameInput") as HTMLInputElement)
            ?.value,
          8,
        ),
      ),
    );
    // FrequencyPulse decoder uses Goertzel/FFT which requires sine waves
    // Non-sine waveforms have harmonic content that breaks frequency detection
    const waveformShape = "sine";

    const nyquist = dsp.sampleRate / 2;
    const clippedBase = Math.min(Math.max(0, base), nyquist);
    const maxBits = Math.max(1, Math.floor((nyquist - clippedBase) / step) + 1);
    const clippedBitsPerFrame = Math.min(bitsPerFrame, maxBits);

    return {
      pulseFrequency: clippedBase,
      frequencyStep: step,
      frameSize: frame,
      detectionThreshold,
      fftPadFactor,
      bitsPerFrame: clippedBitsPerFrame,
      useGoertzel: !!(
        document.getElementById("fp_useGoertzel") as HTMLInputElement
      )?.checked,
      waveformShape,
    };
  }

  const waveformShape = (
    (document.getElementById("shapeInput_visible") as HTMLInputElement)
      ?.value || "sine"
  ).toLowerCase();

  return {
    pulseFrequency: Number(pulseFreqInput.value) || 1000,
    frequencyStep: 1000,
    frameSize: Number(frameSizeInput.value) || 1024,
    waveformShape,
  };
}

function syncEncoderFieldsFromUI() {
  const pulseVisible = document.getElementById(
    "pulseFreqInput_visible",
  ) as HTMLInputElement | null;
  const frameVisible = document.getElementById(
    "frameSizeInput_visible",
  ) as HTMLInputElement | null;
  if (pulseVisible) pulseFreqInput.value = pulseVisible.value;
  if (frameVisible) frameSizeInput.value = frameVisible.value;
}

function createEncoder() {
  syncEncoderFieldsFromUI();
  const options = getProcessorOptions();
  switch (activeEncoderType()) {
    case "frequencyPulse":
      return new FrequencyPulseEncoder(dsp, dsp.sampleRate, {
        pulseFrequency: options.pulseFrequency,
        frequencyStep: options.frequencyStep,
        frameSize: options.frameSize,
        bitsPerFrame: options.bitsPerFrame,
        waveformShape: options.waveformShape as any,
      });
    case "basic":
    default:
      return new BasicEncoder(dsp, dsp.sampleRate, {
        pulseFrequency: options.pulseFrequency,
        frameSize: options.frameSize,
        waveformShape: options.waveformShape as any,
      });
  }
}

function createDecoder(searchSync = false) {
  syncEncoderFieldsFromUI();
  const options = getProcessorOptions();
  switch (activeEncoderType()) {
    case "frequencyPulse":
      return new FrequencyPulseDecoder(dsp, dsp.sampleRate, {
        pulseFrequency: options.pulseFrequency,
        frequencyStep: options.frequencyStep,
        frameSize: options.frameSize,
        bitsPerFrame: options.bitsPerFrame,
        detectionThreshold: options.detectionThreshold,
        fftPadFactor: options.fftPadFactor,
        useGoertzel: options.useGoertzel,
        searchSync,
      });
    case "basic":
    default:
      return new BasicDecoder(dsp, dsp.sampleRate, {
        frameSize: options.frameSize,
      });
  }
}

function renderEncoderOptions() {
  encoderOptionsContainer.innerHTML = "";

  const makeField = (id: string, label: string, value: string) => {
    const wrap = document.createElement("label");
    wrap.innerHTML = label;
    const input = document.createElement("input");
    input.type = "text";
    input.inputMode = "numeric";
    input.id = id;
    input.value = value;
    wrap.appendChild(input);
    return wrap;
  };

  const makeSelectField = (
    id: string,
    label: string,
    value: string,
    options: string[],
  ) => {
    const wrap = document.createElement("label");
    wrap.innerHTML = label;
    const select = document.createElement("select");
    select.id = id;
    options.forEach((optionValue) => {
      const option = document.createElement("option");
      option.value = optionValue;
      option.textContent = optionValue;
      if (optionValue === value) option.selected = true;
      select.appendChild(option);
    });
    wrap.appendChild(select);
    return wrap;
  };

  if (encoderSelect.value === "basic") {
    encoderOptionsContainer.appendChild(
      makeField("pulseFreqInput_visible", "Pulse Freq (Hz)", pulseFreqInput.value),
    );
    encoderOptionsContainer.appendChild(
      makeField("frameSizeInput_visible", "Frame Size", frameSizeInput.value),
    );
    encoderOptionsContainer.appendChild(
      makeSelectField("shapeInput_visible", "Waveform", "sine", [
        "sine",
        "square",
        "triangle",
        "sawtooth",
      ]),
    );
  } else if (encoderSelect.value === "frequencyPulse") {
    encoderOptionsContainer.appendChild(
      makeField("fp_baseFreqInput", "Base Freq (Hz)", pulseFreqInput.value),
    );
    encoderOptionsContainer.appendChild(
      makeField("fp_freqStepInput", "Freq Step (Hz)", "1000"),
    );
    encoderOptionsContainer.appendChild(
      makeField("fp_frameSizeInput", "Frame Size", frameSizeInput.value),
    );
    const shapeNote = document.createElement("label");
    shapeNote.innerHTML = "<strong>Waveform:</strong> <em>sine only (decoder requires sinusoids)</em>";
    shapeNote.style.fontSize = "0.85em";
    shapeNote.style.color = "#999";
    encoderOptionsContainer.appendChild(shapeNote);
    encoderOptionsContainer.appendChild(
      makeField("fp_detectionThreshold", "Detection Threshold", "1.05"),
    );
    encoderOptionsContainer.appendChild(
      makeField("fp_fftPadFactor", "FFT Pad Factor", "1"),
    );
    encoderOptionsContainer.appendChild(
      makeField("fp_bitsPerFrameInput", "Bits per Frame", "8"),
    );
  }

  encoderOptionsContainer.querySelectorAll("input").forEach((input) => {
    input.addEventListener("change", () => {
      if (input.id === "pulseFreqInput_visible") {
        pulseFreqInput.value = input.value;
      } else if (input.id === "frameSizeInput_visible") {
        frameSizeInput.value = input.value;
      }
    });
  });
}

encoderSelect.addEventListener("change", renderEncoderOptions);

frameScrubber?.addEventListener("input", () => {
  inspectorSelectedFrame = Number(frameScrubber.value);
  if (currentSamples) renderFrameInspector(currentSamples);
});
frameInspectorOverviewCanvas?.addEventListener("click", (event) => {
  if (!currentSamples || frameInspectorCount <= 0) return;
  const rect = frameInspectorOverviewCanvas.getBoundingClientRect();
  const x = Math.max(0, Math.min(rect.width, event.clientX - rect.left));
  inspectorSelectedFrame = Math.floor((x / rect.width) * frameInspectorCount);
  inspectorSelectedFrame = Math.max(
    0,
    Math.min(frameInspectorCount - 1, inspectorSelectedFrame),
  );
  if (frameScrubber) {
    frameScrubber.value = String(inspectorSelectedFrame);
  }
  renderFrameInspector(currentSamples);
});

renderEncoderOptions();

function setTestText() {
  const sample = "HELLO DSP WORKSHOP";
  textInput.value = sample;
  log(`🧪 Test text set: "${sample}"`);
}

async function encodeTextAndPlay() {
  const text = textInput.value.trim();
  if (!text) {
    log("⚠️ Enter text before encoding.");
    return;
  }

  log(`📝 Encoding text: "${text}"`);
  const bytes = new TextEncoder().encode(text);
  const encoder = createEncoder();
  const decoder = createDecoder();
  currentSamples = encoder.encode(bytes);
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  const decodeResult = decoder.decode(currentSamples);
  const decodedText = new TextDecoder().decode(decodeResult.bytes);
  decodedOutput.value = decodedText;
  lastEncodedSourceName = "message.txt";
  lastDecodedBytes = decodeResult.bytes;
  lastDecodedFileName = "message.txt";
  lastEncodeMetadata = buildEncodeMetadata(bytes.length, "message.txt");

  updatePreviewFromSignal("encode-text", {
    title: "Text encoded to WAV",
    badge: "Encode · Text",
    stats: [{ label: "Payload", value: `${bytes.length} bytes` }],
    content: `Source text:\n${text}\n\nRound-trip decode:\n${decodedText}`,
    hasData: true,
  });

  log(`✅ Decoded back: "${decodedText}"`, decodeResult.debug ?? null);

  audioIO.play(currentSamples).catch((err) => {
    log(`❌ Playback failed: ${err}`);
  });
}

async function encodeFileToWav(file?: File) {
  const target = file ?? pendingEncodeFile ?? textFileInput.files?.[0];
  if (!target) {
    log("⚠️ No file selected.");
    return;
  }

  const buffer = await target.arrayBuffer();
  const bytes = new Uint8Array(buffer);
  log(`📦 Encoding file ${target.name} to WAV… (${bytes.length} bytes)`);

  const encoder = createEncoder();
  currentSamples = encoder.encode(bytes);
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  lastEncodedSourceName = target.name;
  lastDecodedBytes = bytes;
  lastDecodedFileName = target.name;
  lastEncodeMetadata = buildEncodeMetadata(bytes.length, target.name);

  const wavName = `encoded_${target.name}.wav`;
  downloadCurrentWav(wavName, lastEncodeMetadata);

  updatePreviewFromSignal("encode-file", {
    title: "File encoded to WAV",
    badge: "Encode · Binary",
    stats: [
      { label: "File", value: target.name },
      { label: "Payload", value: formatBytes(bytes.length) },
      { label: "Output", value: wavName },
    ],
    content: bytesPreview(bytes),
    hasData: true,
  });

  log(`✅ Encoded ${target.name} → ${wavName}`);
}

function formatDecodedBytes(bytes: Uint8Array): string {
  const decodedText = new TextDecoder().decode(bytes);
  const hasReplacement = decodedText.includes("�");
  const hasControl = /[\x00-\x08\x0E-\x1F]/.test(decodedText);
  if (hasReplacement || hasControl) {
    return bytesPreview(bytes);
  }
  return decodedText;
}

function decodeLastSignal() {
  if (!currentSamples) {
    log("⚠️ No signal available to decode.");
    return;
  }

  const decoder = createDecoder();
  const decodeResult = decoder.decode(currentSamples);
  lastDecodedBytes = decodeResult.bytes;
  const formatted = formatDecodedBytes(decodeResult.bytes);
  decodedOutput.value = formatted;

  updatePreviewFromSignal("verify", {
    title: "Signal verified (decode)",
    badge: "Encode · Verify",
    stats: [
      {
        label: "Recovered",
        value: formatBytes(decodeResult.bytes.length),
      },
    ],
    content: formatted,
    hasData: true,
  });

  log(
    `🔎 Decoded last signal (${decodeResult.bytes.length} bytes)`,
    decodeResult.debug ?? null,
  );
}

async function decodeWavFile(file?: File) {
  const target = file ?? pendingDecodeFile ?? wavFileInput.files?.[0];
  if (!target) {
    log("⚠️ No WAV file selected.");
    return;
  }

  log(`🎧 Decoding WAV ${target.name}…`);
  const { samples, metadata } = await audioIO.importWAVWithMetadata(target);
  currentSamples = samples;
  currentViewport = { startSample: 0, endSample: samples.length };
  updateAllViews(currentSamples);

  if (metadata) {
    applyMetadataToUI(metadata);
    log(`📎 Loaded embedded modem settings (${metadata.encoder})`);
  } else {
    log(
      "⚠️ No embedded settings in WAV — using current modem type and settings above.",
    );
  }

  const tryDecode = (searchSync: boolean) => {
    const decoder = createDecoder(searchSync);
    return decoder.decode(currentSamples!);
  };

  let decodeResult = tryDecode(false);
  if (!metadata) {
    const withSync = tryDecode(true);
    const scoreWithout = scoreDecodedBytes(decodeResult.bytes);
    const scoreWith = scoreDecodedBytes(withSync.bytes);
    if (scoreWith > scoreWithout) {
      decodeResult = withSync;
    }
  }

  let bytes = decodeResult.bytes;
  const payloadBytes = metadata?.payloadBytes;
  if (payloadBytes != null && payloadBytes > 0 && payloadBytes < bytes.length) {
    bytes = bytes.slice(0, payloadBytes);
  }

  lastDecodedBytes = bytes;
  lastDecodedFileName =
    metadata?.sourceName ?? inferDecodedFilename(target.name);

  const formatted = formatDecodedBytes(bytes);
  decodedOutput.value = formatted;

  updatePreviewFromSignal("decode-wav", {
    title: "WAV decoded to file",
    badge: metadata ? "Decode · Auto-config" : "Decode · Manual",
    stats: [
      { label: "Input", value: target.name },
      { label: "Modem", value: metadata?.encoder ?? activeEncoderType() },
      { label: "Recovered", value: formatBytes(bytes.length) },
      { label: "Save as", value: lastDecodedFileName },
    ],
    content: formatted,
    hasData: true,
  });

  if (bytes.every((b) => b === 0)) {
    log(
      "❌ Decoded data is all zeros — check that modem type matches the encoder (Basic vs Frequency Pulse).",
    );
  } else {
    log(`✅ Decoded WAV → ${bytes.length} bytes`, decodeResult.debug ?? null);
  }
}

function downloadDecodedFile() {
  if (!lastDecodedBytes?.length) {
    log("⚠️ Nothing to download — decode a WAV first.");
    return;
  }
  downloadBytes(lastDecodedBytes, lastDecodedFileName);
  log(`💾 Downloaded ${lastDecodedFileName} (${lastDecodedBytes.length} bytes)`);
}

function updateVisualizations(samples: Float32Array | null) {
  if (!samples) return;

  visualizer.drawWaveform(waveformCanvas, samples, "#5b8def", currentViewport);
  visualizer.drawSpectrogram(
    spectrogramCanvas,
    samples,
    dsp,
    {
      fftSize: spectrogramFftSize,
      colormap: spectrogramColormap,
      minDb: -80,
      maxDb: -20,
    },
    currentViewport,
  );
  visualizer.drawMagnitudeSpectrum(spectrumCanvas, samples, dsp, 2048);

  if (modulator.lastIQSymbols && modulator.lastIQSymbols.length > 0) {
    visualizer.drawConstellation(constellationCanvas, modulator.lastIQSymbols);
  }
}

function updateAllViews(samples: Float32Array | null) {
  if (!samples) return;
  updateVisualizations(samples);
  updateSplitCarrierView();
  updateEnergyView();
  try {
    renderFrameInspector(samples);
  } catch (err) {
    console.warn("Frame inspector error:", err);
  }
}

function renderFrameInspector(samples: Float32Array) {
  if (!frameInspectorCanvas || !frameInspectorOverviewCanvas) return;
  const show = (document.getElementById("fp_showInspector") as HTMLInputElement)
    ?.checked;
  if (!show) {
    const clearCanvas = (canvas: HTMLCanvasElement) => {
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.clearRect(0, 0, canvas.width, canvas.height);
    };
    clearCanvas(frameInspectorCanvas);
    clearCanvas(frameInspectorOverviewCanvas);
    return;
  }

  const opts = getProcessorOptions() as Record<string, any>;
  const frameSize = (opts.frameSize as number) || 1024;
  const bitsPerFrame = (opts.bitsPerFrame as number) || 8;
  const step = frameSize;
  const maxFrames =
    samples.length > 0 ? Math.floor((samples.length - 1) / step) + 1 : 0;
  const count = Math.min(128, maxFrames);
  frameInspectorCount = count;
  const selectedIndex =
    count > 0 ? Math.min(count - 1, inspectorSelectedFrame) : 0;
  inspectorSelectedFrame = Math.max(0, selectedIndex);

  frameScrubber?.setAttribute("max", String(Math.max(0, count - 1)));
  frameScrubber?.setAttribute("value", String(inspectorSelectedFrame));
  if (frameScrubber) frameScrubber.disabled = count <= 1;
  if (frameLabel) {
    frameLabel.textContent = `${inspectorSelectedFrame + 1} / ${count}`;
  }

  const detectionThreshold = (opts.detectionThreshold as number) || 1.05;
  const fftPadFactor = (opts.fftPadFactor as number) || 1;
  const useGoertzel = !!opts.useGoertzel;
  const pulseFrequency = (opts.pulseFrequency as number) || 1000;
  const frequencyStep = (opts.frequencyStep as number) || 1000;

  const detectScore = (frame: Float32Array, freq: number) => {
    const n = frame.length;
    const windowed = dsp.applyWindow(frame, "hann");
    const eps = 1e-12;
    if (useGoertzel) {
      const resolution = dsp.sampleRate / n;
      const targetMag = dsp.goertzel(windowed, freq);
      let sum = 0;
      let cnt = 0;
      const w = 3;
      for (let i = 1; i <= w; i++) {
        const f1 = freq - i * resolution;
        const f2 = freq + i * resolution;
        if (f1 > 0) {
          sum += dsp.goertzel(windowed, f1);
          cnt++;
        }
        if (f2 < dsp.sampleRate / 2) {
          sum += dsp.goertzel(windowed, f2);
          cnt++;
        }
      }
      const local = cnt > 0 ? sum / cnt : eps;
      return targetMag / (local + eps);
    }

    let desired = n * fftPadFactor;
    let fftSizeLocal = 1;
    while (fftSizeLocal < desired) fftSizeLocal <<= 1;
    const padded = new Float32Array(fftSizeLocal);
    padded.set(frame.subarray(0, n));
    const windowedPadded = dsp.applyWindow(padded, "hann");
    const { magnitude } = dsp.fft(windowedPadded);
    const bin = Math.round((freq * fftSizeLocal) / dsp.sampleRate);
    if (bin < 0 || bin >= magnitude.length) return 0;
    const w = 3;
    let sum = 0;
    let countNoise = 0;
    const start = Math.max(0, bin - w);
    const end = Math.min(magnitude.length - 1, bin + w);
    for (let i = start; i <= end; i++) {
      if (i === bin) continue;
      sum += magnitude[i];
      countNoise++;
    }
    const localMean = countNoise > 0 ? sum / countNoise : eps;
    return magnitude[bin] / (localMean + eps);
  };

  type FrameInfo = {
    offset: number;
    length: number;
    score: number;
    bits: number[];
    scores: number[];
    freqs: number[];
  };

  const frameInfos: FrameInfo[] = [];
  for (let f = 0; f < count; f++) {
    const offset = f * step;
    const end = Math.min(offset + frameSize, samples.length);
    const frame = samples.slice(offset, end);
    const paddedFrame =
      frame.length < frameSize
        ? new Float32Array(frameSize).map((_, i) => frame[i] || 0)
        : frame;

    const bits: number[] = [];
    const scores: number[] = [];
    const freqs: number[] = [];
    let totalScore = 0;

    for (let bitIndex = 0; bitIndex < bitsPerFrame; bitIndex++) {
      const freq = pulseFrequency + bitIndex * frequencyStep;
      freqs.push(freq);
      const score = detectScore(paddedFrame, freq);
      scores.push(score);
      const bit = score > detectionThreshold ? 1 : 0;
      bits.push(bit);
      totalScore += score;
    }

    frameInfos.push({
      offset,
      length: end - offset,
      score: totalScore,
      bits,
      scores,
      freqs,
    });
  }

  const drawOverview = () => {
    const canvas = frameInspectorOverviewCanvas;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const ow = canvas.width;
    const oh = canvas.height;
    ctx.fillStyle = "#08080c";
    ctx.fillRect(0, 0, ow, oh);

    if (samples.length > 1) {
      ctx.strokeStyle = "#5b8def";
      ctx.lineWidth = 1;
      ctx.beginPath();
      const full = samples.length - 1;
      for (let x = 0; x < ow; x++) {
        const sampleIndex = Math.min(full, Math.floor((x / ow) * full));
        const y = oh / 2 - samples[sampleIndex] * (oh / 2) * 0.9;
        if (x === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    for (let i = 0; i < frameInfos.length; i++) {
      const frame = frameInfos[i];
      const x0 = Math.floor((frame.offset / samples.length) * ow);
      const x1 = Math.floor(
        ((frame.offset + frame.length) / samples.length) * ow,
      );
      ctx.fillStyle = "rgba(91, 141, 239, 0.08)";
      ctx.fillRect(x0, 0, Math.max(2, x1 - x0), oh);
      if (i === inspectorSelectedFrame) {
        ctx.strokeStyle = "#fbbf24";
        ctx.lineWidth = 2;
        ctx.strokeRect(x0 + 0.5, 0.5, Math.max(2, x1 - x0) - 1, oh - 1);
      }
    }
  };

  const drawFrameDetail = () => {
    const canvas = frameInspectorCanvas;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const cw = canvas.width;
    const ch = canvas.height;
    ctx.fillStyle = "#08080c";
    ctx.fillRect(0, 0, cw, ch);

    const selected = frameInfos[inspectorSelectedFrame] || frameInfos[0];
    if (!selected) return;
    const frameStart = selected.offset;
    const frameEnd = Math.min(selected.offset + frameSize, samples.length);
    const frame = samples.slice(frameStart, frameEnd);
    const paddedFrame =
      frame.length < frameSize
        ? new Float32Array(frameSize).map((_, i) => frame[i] || 0)
        : frame;

    const waveformTop = 12;
    const waveformHeight = 90;
    const fftTop = waveformTop + waveformHeight + 16;
    const fftHeight = 90;

    ctx.fillStyle = "#122136";
    ctx.fillRect(0, waveformTop, cw, waveformHeight);
    ctx.strokeStyle = "#7cc6ff";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < paddedFrame.length; i++) {
      const x = (i / paddedFrame.length) * cw;
      const y =
        waveformTop +
        waveformHeight / 2 -
        paddedFrame[i] * (waveformHeight / 2) * 0.9;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    ctx.fillStyle = "#ccc";
    ctx.font = "12px monospace";
    ctx.fillText(`Bits: ${selected.bits?.join("") ?? "n/a"}`, 10, fftTop - 4);

    const fftSizeLocal = Math.min(1024, paddedFrame.length);
    const fftPadded = new Float32Array(fftSizeLocal);
    fftPadded.set(paddedFrame.subarray(0, fftSizeLocal));
    const windowed = dsp.applyWindow(fftPadded, "hann");
    const { magnitude } = dsp.fft(windowed);
    const half = magnitude.length / 2;
    let maxMag = 1e-9;
    for (let i = 1; i < half; i++) maxMag = Math.max(maxMag, magnitude[i]);

    ctx.fillStyle = "#122136";
    ctx.fillRect(0, fftTop, cw, fftHeight);
    ctx.strokeStyle = "#a0f0a0";
    ctx.beginPath();
    for (let i = 1; i < half; i++) {
      const x = (i / half) * cw;
      const y =
        fftTop + fftHeight - (magnitude[i] / maxMag) * (fftHeight - 16) - 8;
      if (i === 1) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
  };

  drawOverview();
  drawFrameDetail();

  const selected = frameInfos[inspectorSelectedFrame] || frameInfos[0];
  if (frameInspectorDetails && selected) {
    frameInspectorDetails.innerHTML = [
      `Frame ${inspectorSelectedFrame + 1} / ${frameInfos.length}`,
      `Bits: ${selected.bits.join("")}`,
      `Score: ${selected.score.toFixed(2)}`,
    ]
      .map((line) => `<div>${line}</div>`)
      .join("");
  }
}

function generateBPSKTest() {
  log("🔧 Generating BPSK test signal…");
  const bits = [
    1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1,
    1, 0, 0,
  ];
  const carrierFreq = 2000;
  const symbolRate = 200;
  const duration = bits.length / symbolRate;

  currentSamples = modulator.bpskModulate(
    bits,
    carrierFreq,
    symbolRate,
    duration,
  );
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  updatePreviewFromSignal("generate", {
    title: "BPSK test signal",
    badge: "Signal Lab · BPSK",
    stats: [
      { label: "Bits", value: String(bits.length) },
      { label: "Carrier", value: `${carrierFreq} Hz` },
    ],
    content: `Bit pattern:\n${bits.join("")}`,
    hasData: true,
  });

  log(`✅ Generated ${currentSamples.length} samples`);
}

function generateChirp() {
  log("🔊 Generating frequency sweep…");
  currentSamples = dsp.generateChirp(200, 8000, 2.0);
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  updatePreviewFromSignal("generate", {
    title: "Chirp sweep",
    badge: "Signal Lab · Chirp",
    stats: [{ label: "Range", value: "200 → 8000 Hz" }],
    content: "Linear frequency sweep — check spectrogram for rising line.",
    hasData: true,
  });
}

function generateMultiTone() {
  log("🎵 Generating multi-tone test…");
  const freqs = [500, 1500, 3000, 6000];
  const tones = freqs.map((f) => dsp.generateCarrier(f, 1.0, 0.3));
  currentSamples = dsp.mix(tones);
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  updatePreviewFromSignal("generate", {
    title: "Multi-tone signal",
    badge: "Signal Lab · Tones",
    stats: [{ label: "Frequencies", value: freqs.map((f) => `${f} Hz`).join(", ") }],
    content: "Four simultaneous carriers — FFT should show four peaks.",
    hasData: true,
  });
}

async function play() {
  if (!currentSamples) {
    log("⚠️ No signal to play.");
    return;
  }
  log("▶️ Playing…");
  await audioIO.play(currentSamples);
}

async function record() {
  log("🎙️ Recording for 3 seconds…");
  const recorded = await audioIO.record(3);
  currentSamples = recorded;
  currentViewport = { startSample: 0, endSample: recorded.length };
  updateAllViews(recorded);

  updatePreviewFromSignal("record", {
    title: "Recorded audio",
    badge: "Signal Lab · Mic",
    content: "Live microphone capture loaded into visualizer.",
    hasData: true,
  });

  log(`✅ Recorded ${recorded.length} samples`);
}

function exportWAV() {
  if (!currentSamples) {
    log("⚠️ No signal to export");
    return;
  }
  downloadCurrentWav(`modem_signal_${Date.now()}.wav`);
  log("💾 WAV exported");
}

async function importWAV(file: File) {
  log(`📂 Importing ${file.name}…`);
  const samples = await audioIO.importWAV(file);
  currentSamples = samples;
  currentViewport = { startSample: 0, endSample: samples.length };
  updateAllViews(samples);

  updatePreviewFromSignal("import", {
    title: "Imported WAV",
    badge: "Signal Lab · Import",
    stats: [{ label: "File", value: file.name }],
    content: `Loaded ${file.name} for analysis. Use zoom/pan on waveform and spectrogram.`,
    hasData: true,
  });

  log(`✅ Imported ${samples.length} samples at ${dsp.sampleRate}Hz`);
}

function analyze() {
  if (!currentSamples) {
    log("⚠️ No signal to analyze");
    return;
  }

  const fftSize = 2048;
  const analysis = dsp.analyzeFrequency(currentSamples, fftSize);

  log(`📊 FFT Analysis (${fftSize} points):`, {
    peaks: analysis.peaks.map(
      (p) => `${p.freq.toFixed(0)}Hz @ ${p.magnitude.toFixed(3)}`,
    ),
    noiseFloor: analysis.noiseFloor.toFixed(6),
  });
}

function generateMultiCarrierSignal() {
  log(`🎵 Generating ${currentCarrierFrequencies.length} carrier test signal…`);

  const duration = 2.0;
  const signals: Float32Array[] = [];

  for (let c = 0; c < currentCarrierFrequencies.length; c++) {
    const freq = currentCarrierFrequencies[c];
    const bits: number[] = [];
    for (let i = 0; i < 20; i++) {
      bits.push((c + i) % 2 === 0 ? 1 : 0);
    }
    const symbolRate = 50;
    signals.push(modulator.bpskModulate(bits, freq, symbolRate, duration));
  }

  currentSamples = dsp.mix(signals);
  currentViewport = { startSample: 0, endSample: currentSamples.length };
  updateAllViews(currentSamples);

  updatePreviewFromSignal("generate", {
    title: "Multi-carrier composite",
    badge: "Signal Lab · OFDM-style",
    stats: [
      {
        label: "Carriers",
        value: currentCarrierFrequencies.map((f) => `${f} Hz`).join(", "),
      },
    ],
    content: "Independent BPSK modulators per carrier, mixed together.",
    hasData: true,
  });
}

function generateOFDMSignal() {
  log(`📡 Generating OFDM-style signal…`);

  const symbolDuration = 0.02;
  const numSymbols = 10;
  const samplesPerSymbol = Math.floor(dsp.sampleRate * symbolDuration);
  const totalSamples = numSymbols * samplesPerSymbol;
  const output = new Float32Array(totalSamples);

  const symbols: { i: number; q: number }[][] = [];
  for (let c = 0; c < currentCarrierFrequencies.length; c++) {
    const carrierSymbols = [];
    for (let s = 0; s < numSymbols; s++) {
      const phase = (Math.floor(Math.random() * 4) * Math.PI) / 2;
      carrierSymbols.push({ i: Math.cos(phase), q: Math.sin(phase) });
    }
    symbols.push(carrierSymbols);
  }

  for (let sym = 0; sym < numSymbols; sym++) {
    const startSample = sym * samplesPerSymbol;
    for (let s = 0; s < samplesPerSymbol; s++) {
      const t = sym * symbolDuration + s / dsp.sampleRate;
      let sample = 0;
      for (let c = 0; c < currentCarrierFrequencies.length; c++) {
        const freq = currentCarrierFrequencies[c];
        const symVal = symbols[c][sym];
        sample += symVal.i * Math.sin(2 * Math.PI * freq * t);
        sample += symVal.q * Math.cos(2 * Math.PI * freq * t);
      }
      output[startSample + s] = sample / currentCarrierFrequencies.length;
    }
  }

  currentSamples = output;
  currentViewport = { startSample: 0, endSample: output.length };
  updateAllViews(output);

  updatePreviewFromSignal("generate", {
    title: "OFDM test signal",
    badge: "Signal Lab · OFDM",
    stats: [
      { label: "Symbols", value: String(numSymbols) },
      { label: "Subcarriers", value: String(currentCarrierFrequencies.length) },
    ],
    content: "QPSK symbols on parallel subcarriers with symbol-time multiplexing.",
    hasData: true,
  });
}

function updateSplitCarrierView() {
  if (!currentSamples) return;
  visualizer.drawSplitCarriers(
    splitCarriersCanvas,
    currentSamples,
    dsp,
    currentCarrierFrequencies,
    carrierBandwidth,
  );
}

function updateEnergyView() {
  if (!currentSamples) return;
  visualizer.drawCarrierEnergy(
    energyCanvas,
    currentSamples,
    dsp,
    currentCarrierFrequencies,
    carrierBandwidth,
  );
}

function setupKeyboardShortcuts() {
  window.addEventListener("keydown", (e) => {
    const target = e.target as HTMLElement | null;
    if (
      target &&
      (target.tagName === "INPUT" ||
        target.tagName === "TEXTAREA" ||
        target.isContentEditable)
    ) {
      return;
    }

    if (e.key === "+" || e.key === "=") {
      e.preventDefault();
      zoomWaveform(0.5);
    } else if (e.key === "-" || e.key === "_") {
      e.preventDefault();
      zoomWaveform(2);
    } else if (e.key === "0") {
      e.preventDefault();
      resetZoom();
    }
  });
}

function bindControls() {
  document.querySelectorAll(".mode-tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      setMode((tab as HTMLElement).dataset.mode as AppMode);
    });
  });

  document.getElementById("zoomInBtn")?.addEventListener("click", () => zoomWaveform(0.5));
  document.getElementById("zoomOutBtn")?.addEventListener("click", () => zoomWaveform(2));
  document.getElementById("resetZoomBtn")?.addEventListener("click", resetZoom);

  document.getElementById("colormapSelect")?.addEventListener("change", (e) => {
    spectrogramColormap = (e.target as HTMLSelectElement).value as typeof spectrogramColormap;
    updateAllViews(currentSamples);
  });
  document.getElementById("fftSizeSelectSpec")?.addEventListener("change", (e) => {
    spectrogramFftSize = parseInt((e.target as HTMLSelectElement).value);
    updateAllViews(currentSamples);
  });

  document.getElementById("genBPSKBtn")?.addEventListener("click", generateBPSKTest);
  document.getElementById("genCarrierBtn")?.addEventListener("click", generateMultiTone);
  document.getElementById("genChirpBtn")?.addEventListener("click", generateChirp);
  document.getElementById("genMultiCarrierBtn")?.addEventListener("click", generateMultiCarrierSignal);
  document.getElementById("genOFDMBtn")?.addEventListener("click", generateOFDMSignal);
  document.getElementById("playBtn")?.addEventListener("click", play);
  document.getElementById("recordBtn")?.addEventListener("click", record);
  document.getElementById("exportBtn")?.addEventListener("click", exportWAV);
  document.getElementById("analyzeBtn")?.addEventListener("click", analyze);

  document.getElementById("updateCarriersBtn")?.addEventListener("click", () => {
    const input = (document.getElementById("carrierFreqs") as HTMLInputElement).value;
    currentCarrierFrequencies = input
      .split(",")
      .map((s) => parseInt(s.trim()))
      .filter((n) => !isNaN(n));
    log(`Updated carriers: ${currentCarrierFrequencies.join(", ")} Hz`);
    if (currentSamples) {
      updateSplitCarrierView();
      updateEnergyView();
    }
  });

  document.getElementById("encodeTextBtn")?.addEventListener("click", encodeTextAndPlay);
  document.getElementById("setTestTextBtn")?.addEventListener("click", setTestText);
  document.getElementById("decodeTextBtn")?.addEventListener("click", decodeLastSignal);
  document.getElementById("encodeFileBtn")?.addEventListener("click", () => encodeFileToWav());
  document.getElementById("exportEncodeBtn")?.addEventListener("click", () => {
    const name = lastEncodedSourceName
      ? `encoded_${lastEncodedSourceName}.wav`
      : `modem_signal_${Date.now()}.wav`;
    downloadCurrentWav(name, lastEncodeMetadata ?? undefined);
  });
  document.getElementById("decodeWavBtn")?.addEventListener("click", () => decodeWavFile());
  document.getElementById("downloadDecodedBtn")?.addEventListener("click", downloadDecodedFile);
  document.getElementById("exportDecodedWavBtn")?.addEventListener("click", exportWAV);

  setupFileDrop(
    document.getElementById("labFileDrop")!,
    document.getElementById("importBtn") as HTMLInputElement,
    document.getElementById("labFileName"),
    (file) => importWAV(file),
  );
  setupFileDrop(
    document.getElementById("encodeFileDrop")!,
    textFileInput,
    document.getElementById("encodeFileName"),
    (file) => {
      pendingEncodeFile = file;
    },
  );
  setupFileDrop(
    document.getElementById("decodeFileDrop")!,
    wavFileInput,
    document.getElementById("decodeFileName"),
    (file) => {
      pendingDecodeFile = file;
    },
  );
}

bindControls();
setupWaveformPanZoom();
setupKeyboardShortcuts();
setMode("lab");
generateBPSKTest();

log("🎯 Modem Workshop ready — unified UI with context previews.");
