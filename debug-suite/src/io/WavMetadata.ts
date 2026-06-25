export interface ModemWavMetadata {
  v: 1;
  encoder: "basic" | "frequencyPulse";
  options: Record<string, number | boolean>;
  payloadBytes: number;
  sourceName?: string;
}

const META_CHUNK_ID = "mwm ";

export function buildWavWithMetadata(
  samples: Int16Array,
  sampleRate: number,
  metadata: ModemWavMetadata,
): Blob {
  const metaJson = JSON.stringify(metadata);
  const metaBytes = new TextEncoder().encode(metaJson);
  const metaPad = metaBytes.length % 2;
  const metaChunkBody = metaBytes.length + metaPad;

  const dataBytes = samples.length * 2;
  const riffSize = 4 + (8 + 16) + (8 + metaChunkBody) + (8 + dataBytes);
  const buffer = new ArrayBuffer(8 + riffSize);
  const view = new DataView(buffer);

  const writeString = (offset: number, str: string) => {
    for (let i = 0; i < str.length; i++) {
      view.setUint8(offset + i, str.charCodeAt(i));
    }
  };

  let offset = 0;
  writeString(offset, "RIFF");
  offset += 4;
  view.setUint32(offset, riffSize, true);
  offset += 4;
  writeString(offset, "WAVE");
  offset += 4;

  writeString(offset, "fmt ");
  offset += 4;
  view.setUint32(offset, 16, true);
  offset += 4;
  view.setUint16(offset, 1, true);
  offset += 2;
  view.setUint16(offset, 1, true);
  offset += 2;
  view.setUint32(offset, sampleRate, true);
  offset += 4;
  view.setUint32(offset, sampleRate * 2, true);
  offset += 4;
  view.setUint16(offset, 2, true);
  offset += 2;
  view.setUint16(offset, 16, true);
  offset += 2;

  writeString(offset, META_CHUNK_ID);
  offset += 4;
  view.setUint32(offset, metaChunkBody, true);
  offset += 4;
  for (let i = 0; i < metaBytes.length; i++) {
    view.setUint8(offset + i, metaBytes[i]);
  }
  offset += metaBytes.length + metaPad;

  writeString(offset, "data");
  offset += 4;
  view.setUint32(offset, dataBytes, true);
  offset += 4;
  for (let i = 0; i < samples.length; i++) {
    view.setInt16(offset, samples[i], true);
    offset += 2;
  }

  return new Blob([buffer], { type: "audio/wav" });
}

export function parseWavMetadata(buffer: ArrayBuffer): ModemWavMetadata | null {
  const view = new DataView(buffer);
  if (buffer.byteLength < 12) return null;

  const readFourCC = (pos: number) =>
    String.fromCharCode(
      view.getUint8(pos),
      view.getUint8(pos + 1),
      view.getUint8(pos + 2),
      view.getUint8(pos + 3),
    );

  if (readFourCC(0) !== "RIFF" || readFourCC(8) !== "WAVE") return null;

  let offset = 12;
  while (offset + 8 <= buffer.byteLength) {
    const id = readFourCC(offset);
    const size = view.getUint32(offset + 4, true);
    const dataStart = offset + 8;
    if (id === META_CHUNK_ID && dataStart + size <= buffer.byteLength) {
      const bytes = new Uint8Array(buffer, dataStart, size);
      try {
        const parsed = JSON.parse(new TextDecoder().decode(bytes)) as ModemWavMetadata;
        if (parsed?.v === 1 && parsed.encoder && parsed.payloadBytes > 0) {
          return parsed;
        }
      } catch {
        return null;
      }
    }
    offset = dataStart + size + (size % 2);
  }

  return null;
}

export function scoreDecodedBytes(bytes: Uint8Array): number {
  if (!bytes.length) return 0;
  let nonZero = 0;
  for (const b of bytes) {
    if (b !== 0) nonZero++;
  }
  return nonZero;
}
