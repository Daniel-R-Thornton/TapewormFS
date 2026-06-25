export interface EncodedAudio {
  sampleRate: number;
  channels: Float32Array[];
}

export interface MonoFrame {
  left: Float32Array;
  right: Float32Array;
}

export interface StereoFrame {
  left: Float32Array;
  right: Float32Array;
}
