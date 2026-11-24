import type {WakeOptions, Detection} from '@phonlet-ai/wake-web';

/** Manager shape the hook drives; the Web SDK's WakeManager satisfies it. */
export interface WakeManagerLike {
  readonly state: 'idle'|'starting'|'listening'|'stopping'|'error'|'closed';
  start(): Promise<void>;
  stop(): Promise<void>;
  close(): Promise<void>|void;
}

export interface UseWakeOptions extends WakeOptions {
  /** Called for each detection event while listening. */
  onDetection?: (event: Detection) => void;
  /** Called for manager/engine errors; the hook also exposes them as `error`. */
  onError?: (error: Error) => void;
  audioContext?: AudioContext;
  stream?: MediaStream;
  /** Testing/embedding hook: supply a manager class instead of the Web SDK. */
  factory?: () => Promise<new (options: {
    wake: WakeOptions;
    audioContext?: AudioContext;
    stream?: MediaStream;
    onDetection: (event: Detection) => void;
    onError: (error: Error) => void;
  }) => WakeManagerLike>;
}

export type UseWakeState = 'idle'|'starting'|'listening'|'stopping'|'error';

export interface UseWakeResult {
  readonly state: UseWakeState;
  readonly error: Error | null;
  start(): Promise<void>;
  stop(): Promise<void>;
  /** Stop, then start again: the options are reused, stream state restarts at zero. */
  restart(): Promise<void>;
}

export declare function useWake(options: UseWakeOptions): UseWakeResult;
