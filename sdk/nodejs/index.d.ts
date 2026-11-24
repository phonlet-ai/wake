export type KeywordMode = 'independent' | 'production_arbitrated';
export interface Detection {
  readonly keywordId: string; readonly keyword: string; readonly keywordIndex: number;
  readonly frameIndex: number; readonly sampleIndex: number; readonly timeSeconds: number; readonly sensitivity: number;
}
export interface FinishResult {readonly frames: number; readonly discardedSamples: number}
/** One phrase with its pronunciations and calibrated thresholds, as in a keyword file. */
export interface Phrase {
  name: string; pronunciations: readonly (readonly number[])[]; threshold_loose: number; threshold_strict: number; endpoint: boolean;
  id?: string; metadata?: Readonly<Record<string, unknown>>; sensitivity?: number;
}
/** What the threshold generator writes; "model" binds the phrases to one model by SHA-256. */
export interface KeywordFile {
  model?: Readonly<{id: string; sha256: string}>; keywords: readonly Phrase[]; metadata?: Readonly<Record<string, unknown>>;
}
export class WakeError extends Error {constructor(code:string,message:string); readonly code: string}
/** A model and its phrases: a keyword file path or URL, the parsed keyword file, or a list of phrases. */
export interface WakeOptions {
  model: string | URL | Uint8Array; keywords: string | URL | KeywordFile | readonly Phrase[]; sensitivity?: number; keywordMode?: KeywordMode;
}
export class Wake {
  private constructor();
  static create(options: WakeOptions): Promise<Wake>;
  readonly sampleRate: 16000; readonly frameLength: 512;
  readonly state: 'ready' | 'finished' | 'closed';
  readonly keywords: readonly Readonly<{id: string; name: string; sensitivity: number}>[];
  readonly info: Readonly<{sdkVersion:string; engineAbi:number; engineSha256:string; keywordsSha256:string; keywordMode:KeywordMode; scorer:string; scorerOptions:string; backend:string; modelSha256:string; inventoryId:string; frontendId:string}>;
  process(pcm: Int16Array): readonly Detection[];
  finish(): FinishResult;
  close(): void;
}
export function readWav(bytes: Uint8Array): Int16Array;
