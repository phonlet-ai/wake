// Thin React adapter over the Web SDK's WakeManager. The hook owns at most one
// manager instance; unmount closes it, stop/start reuse the same options with
// fresh stream state, and the wake options are snapshotted on first start
// (changing them requires a remount). The default factory imports the Web SDK
// lazily so environments without Worker/fetch never load it.
import {useCallback, useEffect, useRef, useState} from 'react';

const defaultManagerFactory = async () => {
  const module = await import('@phonlet-ai/wake-web/capture');
  return module.WakeManager;
};

export function useWake(options) {
  if (!options || typeof options !== 'object' || Array.isArray(options)) {
    throw new Error('useWake requires an options object');
  }
  const {onDetection, onError, audioContext, stream, factory, ...wake} = options;
  const create = factory || defaultManagerFactory;

  const [state, setState] = useState('idle');
  const [error, setError] = useState(null);
  const manager = useRef(null);
  const alive = useRef(true);
  const callbacks = useRef({onDetection, onError});
  callbacks.current = {onDetection, onError};
  const configuration = useRef({wake, audioContext, stream});
  // First-start snapshot: later renders cannot silently reconfigure a live manager.
  if (manager.current === null && !configuration.current.captured) {
    configuration.current = {wake, audioContext, stream, captured: true};
  }

  const start = useCallback(async () => {
    if (!manager.current) {
      const Manager = await create();
      const {wake, audioContext, stream} = configuration.current;
      manager.current = new Manager({
        wake, audioContext, stream,
        onDetection: (event) => callbacks.current.onDetection?.(event),
        onError: (error) => {
          if (alive.current) { setState('error'); setError(error); }
          callbacks.current.onError?.(error);
        },
      });
    }
    if (alive.current) { setState('starting'); }
    await manager.current.start();
    if (alive.current) setState(manager.current.state === 'listening' ? 'listening' : manager.current.state);
  }, [create]);

  const stop = useCallback(async () => {
    if (!manager.current) return;
    if (alive.current) setState('stopping');
    await manager.current.stop();
    if (alive.current) setState('idle');
  }, []);

  const restart = useCallback(async () => {
    await stop();
    await start();
  }, [start, stop]);

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      const current = manager.current;
      manager.current = null;
      current?.close();
    };
  }, []);

  return {state, error, start, stop, restart};
}
