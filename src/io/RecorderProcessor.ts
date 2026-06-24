/// <reference lib="webworker" />

declare class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: any);
  process(
    inputs: Float32Array[][],
    outputs: Float32Array[][],
    parameters: Record<string, Float32Array>,
  ): boolean;
}

declare function registerProcessor(
  name: string,
  processorCtor: typeof AudioWorkletProcessor,
): void;

class RecorderProcessor extends AudioWorkletProcessor {
  process(inputs: Float32Array[][]): boolean {
    const input = inputs[0];
    if (input && input[0]) {
      this.port.postMessage(input[0].slice());
    }
    return true;
  }
}

registerProcessor("recorder-processor", RecorderProcessor);
export {};
