import {WakeOptions,Detection} from './index.js';
export interface CaptureOptions {
  wake:WakeOptions; onDetection:(event:Detection)=>void; onError:(error:Error)=>void;
  audioContext?:AudioContext; stream?:MediaStream;
}
export class WakeManager {
  constructor(options:CaptureOptions);
  readonly state:'idle'|'starting'|'listening'|'stopping'|'error'|'closed';
  readonly lastError:Error|null;
  start():Promise<void>; stop():Promise<void>; close():Promise<void>;
}
