import { DSPEngine } from "../core/DSPEngine";
import { parseWavMetadata, type ModemWavMetadata } from "./WavMetadata";

export class AudioIO {
  private dsp: DSPEngine;
  private audioContext: AudioContext | null = null;

  constructor(dsp: DSPEngine) {
    this.dsp = dsp;
  }

  async play(samples: Float32Array): Promise<void> {
    this.audioContext ??= new AudioContext({ sampleRate: this.dsp.sampleRate });

    const buffer = this.audioContext.createBuffer(
      1,
      samples.length,
      this.dsp.sampleRate,
    );
    const copy = new Float32Array(samples);
    buffer.copyToChannel(copy, 0);

    const source = this.audioContext.createBufferSource();
    source.buffer = buffer;
    source.connect(this.audioContext.destination);
    source.start();

    await this.audioContext.resume();
  }

  async record(durationSeconds: number): Promise<Float32Array> {
    this.audioContext ??= new AudioContext({ sampleRate: this.dsp.sampleRate });

    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    const source = this.audioContext.createMediaStreamSource(stream);

    if (this.audioContext.audioWorklet) {
      await this.audioContext.audioWorklet.addModule(
        new URL("./RecorderProcessor.ts", import.meta.url),
      );

      const recorderNode = new AudioWorkletNode(
        this.audioContext,
        "recorder-processor",
        {
          numberOfInputs: 1,
          numberOfOutputs: 0,
          channelCount: 1,
        },
      );

      const chunks: Float32Array[] = [];
      recorderNode.port.onmessage = (event) => {
        chunks.push(event.data as Float32Array);
      };

      source.connect(recorderNode);

      return new Promise((resolve) => {
        window.setTimeout(async () => {
          source.disconnect(recorderNode);
          recorderNode.port.close();
          stream.getTracks().forEach((track) => track.stop());

          const totalLength = chunks.reduce((sum, c) => sum + c.length, 0);
          const recorded = new Float32Array(totalLength);
          let offset = 0;
          for (const chunk of chunks) {
            recorded.set(chunk, offset);
            offset += chunk.length;
          }

          resolve(recorded);
        }, durationSeconds * 1000);

        this.audioContext!.resume().catch(() => undefined);
      });
    }

    const chunks: Blob[] = [];
    const recorder = new MediaRecorder(stream);

    recorder.ondataavailable = (event) => {
      if (event.data.size > 0) {
        chunks.push(event.data);
      }
    };

    recorder.start();

    return new Promise((resolve) => {
      window.setTimeout(async () => {
        recorder.stop();
        recorder.onstop = async () => {
          stream.getTracks().forEach((track) => track.stop());
          const blob = new Blob(chunks, {
            type: chunks[0]?.type ?? "audio/webm",
          });
          const arrayBuffer = await blob.arrayBuffer();
          const audioBuffer =
            await this.audioContext!.decodeAudioData(arrayBuffer);
          resolve(audioBuffer.getChannelData(0).slice());
        };
      }, durationSeconds * 1000);

      this.audioContext!.resume().catch(() => undefined);
    });
  }

  async importWAV(file: File): Promise<Float32Array> {
    const result = await this.importWAVWithMetadata(file);
    return result.samples;
  }

  async importWAVWithMetadata(file: File): Promise<{
    samples: Float32Array;
    metadata: ModemWavMetadata | null;
    sampleRate: number;
  }> {
    const arrayBuffer = await file.arrayBuffer();
    const metadata = parseWavMetadata(arrayBuffer);
    const audioContext = new AudioContext();
    const audioBuffer = await audioContext.decodeAudioData(
      arrayBuffer.slice(0),
    );
    this.dsp.sampleRate = audioBuffer.sampleRate;
    return {
      samples: audioBuffer.getChannelData(0).slice(),
      metadata,
      sampleRate: audioBuffer.sampleRate,
    };
  }
}
