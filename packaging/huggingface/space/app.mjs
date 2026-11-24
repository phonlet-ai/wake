// Phonlet Wake lab: type a phrase, calibrate it with the threshold generator (WebAssembly, in a
// worker), and listen for it with the browser SDK. The SDK and the generator are the files of the
// Phonlet Wake release, served next to this page; the model, its config and the English phrases
// come from the model repo on the Hub. No audio leaves the tab.
import { WakeManager } from './wake/capture.mjs';

const HUB = 'https://huggingface.co/phonletai/wake-en-1/resolve/main';
const MODEL = `${HUB}/phonlet-wake-en-1.pwake`;
const CONFIG = `${HUB}/config.json`;
const KEYWORDS = `${HUB}/phonlet-wake-en-1-keywords.json`;
const MAX_HEARD = 12;

// The engine's phone inventory: id i is PHONES[i] (0 is the blank).
// prettier-ignore
const PHONES = [
  'blank', 'AA', 'AE', 'AH', 'AO', 'AW', 'AY', 'B', 'CH', 'D', 'DH', 'EH', 'ER', 'EY', 'F', 'G',
  'HH', 'IH', 'IY', 'JH', 'K', 'L', 'M', 'N', 'NG', 'OW', 'OY', 'P', 'R', 'S', 'SH', 'T', 'TH',
  'UH', 'UW', 'V', 'W', 'Y', 'Z', 'ZH',
];
const SOURCES = { lexicon: 'dictionary', g2p: 'predicted', explicit: 'your sounds' };

const $ = (id) => document.getElementById(id);
const ui = {
  form: $('add'),
  phrase: $('phrase'),
  add: $('add-button'),
  phrases: $('phrases'),
  sensitivity: $('sensitivity'),
  sensitivityValue: $('sensitivity-value'),
  rate: $('rate'),
  listen: $('listen'),
  download: $('download'),
  level: $('level'),
  status: $('status'),
  heard: $('heard'),
  heardCount: $('heard-count'),
  burst: $('burst'),
};

const state = {
  model: null, // {id, sha256} of the keyword file the page started with
  phrases: [], // {entry, source, fresh}
  sensitivity: 0.5,
  session: null, // {manager, context, stream, analyser, frame}
  starting: false,
  busy: false,
  heard: 0,
};

const spell = (ids) => ids.map((id) => PHONES[id] ?? '?').join(' ');
const megabytes = (bytes) => (bytes / 1048576).toFixed(1);

function say(message, error = false) {
  ui.status.textContent = message;
  ui.status.classList.toggle('is-error', error);
}

function keywordFile() {
  return {
    model: state.model,
    keywords: state.phrases.map((p) => p.entry),
    metadata: {},
  };
}

function render() {
  ui.phrases.replaceChildren(
    ...state.phrases.map((phrase, index) => {
      const item = document.createElement('li');
      if (phrase.fresh) item.classList.add('is-new');
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = phrase.entry.name;
      const sounds = document.createElement('span');
      sounds.className = 'sounds';
      sounds.textContent = spell(phrase.entry.pronunciations[0] ?? []);
      const tag = document.createElement('span');
      const warned = (phrase.entry.metadata?.warnings ?? []).length > 0;
      tag.className = warned ? 'tag warn' : 'tag';
      tag.textContent = warned ? 'check it' : (SOURCES[phrase.source] ?? 'built-in');
      if (warned) tag.title = phrase.entry.metadata.warnings[0].message;
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.textContent = '×';
      remove.setAttribute('aria-label', `Remove “${phrase.entry.name}”`);
      remove.addEventListener('click', () => removePhrase(index));
      item.append(name, sounds, tag, remove);
      return item;
    }),
  );
  const listening = Boolean(state.session);
  ui.listen.textContent = listening ? 'Stop listening' : 'Start listening';
  ui.listen.classList.toggle('is-on', listening);
  ui.listen.disabled = state.starting || (!listening && state.phrases.length === 0);
  ui.download.disabled = state.phrases.length === 0;
  ui.add.disabled = state.busy;
}

// ---------- The threshold generator ----------
let worker = null;
let sequence = 0;
const pending = new Map();

function generate(request) {
  if (!worker) {
    worker = new Worker('./generator/worker.js');
    worker.onmessage = ({ data }) => {
      if (data.progress) {
        const [loaded, total] = data.progress;
        say(`Loading the calibrator · ${megabytes(loaded)} of ${megabytes(total)} MB, once`);
        return;
      }
      const done = pending.get(data.id);
      pending.delete(data.id);
      done?.(data);
    };
    worker.onerror = (event) => {
      for (const done of pending.values()) done({ error: event.message || 'generator failed' });
      pending.clear();
      worker?.terminate();
      worker = null;
    };
  }
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    pending.set(id, (reply) => {
      if (reply.error) reject(new Error(reply.error));
      else if (reply.result?.error) reject(new Error(reply.result.error));
      else resolve(reply.result);
    });
    worker.postMessage({ id, request });
  });
}

async function addPhrase(text) {
  const phrase = text.trim().replace(/\s+/g, ' ');
  if (!phrase || state.busy) return;
  const name = phrase.split('=')[0].trim().toLowerCase();
  if (state.phrases.some((p) => p.entry.name.toLowerCase() === name)) {
    say(`“${name}” is already in the list.`);
    return;
  }
  state.busy = true;
  render();
  say(`Calibrating “${name}”…`);
  try {
    const result = await generate({ command: 'enroll', phrases: [phrase] });
    if (result.model?.sha256 !== state.model.sha256) throw new Error('the generator is for another model');
    const entry = result.keywords?.[0];
    if (!entry) {
      say(result.metadata?.unknown?.[0]?.error ?? `“${name}” could not be calibrated.`, true);
      return;
    }
    state.phrases.forEach((p) => (p.fresh = false));
    state.phrases.push({ entry, source: entry.metadata?.source, fresh: true });
    ui.phrase.value = '';
    const warning = entry.metadata?.warnings?.[0]?.message;
    const listening = Boolean(state.session);
    if (listening) await restart();
    if (!state.session && listening) return; // restart failed and said why
    say(
      warning
        ? `Added “${entry.name}”: ${warning}.`
        : `Added “${entry.name}”. ${state.session ? 'Say it.' : 'Start listening and say it.'}`,
    );
  } catch (error) {
    say(`Could not calibrate “${name}”: ${error.message}`, true);
  } finally {
    state.busy = false;
    render();
  }
}

async function removePhrase(index) {
  const [removed] = state.phrases.splice(index, 1);
  say(`Removed “${removed.entry.name}”.`);
  if (state.session) {
    if (state.phrases.length) await restart();
    else await stop('Stopped: there are no phrases left.');
  }
  render();
}

// ---------- Listening ----------
const supportsSimd = () => {
  try {
    // A minimal module that returns a v128 constant.
    return WebAssembly.validate(
      new Uint8Array([
        0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 22, 1, 20, 0, 253, 12,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11,
      ]),
    );
  } catch {
    return false;
  }
};

async function start() {
  if (state.session || state.starting || !state.phrases.length) return;
  state.starting = true;
  render();
  say('Starting the engine…');
  let context;
  let stream;
  try {
    context = new AudioContext({ sampleRate: 16000 });
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
    const analyser = context.createAnalyser();
    analyser.fftSize = 1024;
    context.createMediaStreamSource(stream).connect(analyser);
    const manager = new WakeManager({
      wake: { model: MODEL, keywords: keywordFile(), sensitivity: state.sensitivity, simd: supportsSimd() },
      audioContext: context,
      stream,
      onDetection: heard,
      onError: (error) => void stop(`Stopped: ${error.message}`, true),
    });
    const session = { manager, context, stream, analyser, frame: 0 };
    state.session = session;
    await manager.start();
    const samples = new Float32Array(analyser.fftSize);
    let level = 0;
    const tick = () => {
      analyser.getFloatTimeDomainData(samples);
      let sum = 0;
      for (const s of samples) sum += s * s;
      const rms = Math.sqrt(sum / samples.length);
      // -60 to -12 dBFS onto 0..1, so room noise stays low and speech fills the range.
      const target = rms > 0.001 ? Math.min(1, Math.max(0, (20 * Math.log10(rms) + 60) / 48)) : 0;
      level += (target - level) * 0.3;
      ui.level.style.width = `${(level * 100).toFixed(1)}%`;
      session.frame = requestAnimationFrame(tick);
    };
    session.frame = requestAnimationFrame(tick);
    const names = state.phrases.slice(0, 3).map((p) => `“${p.entry.name}”`);
    say(`Listening. Say ${names.join(', ')}${state.phrases.length > 3 ? '…' : '.'}`);
  } catch (error) {
    const owned = state.session;
    state.session = null;
    await owned?.manager.close().catch(() => {});
    stream?.getTracks().forEach((track) => track.stop());
    await context?.close().catch(() => {});
    const message =
      error?.name === 'NotAllowedError'
        ? 'Microphone access was blocked. Allow it in the address bar and try again.'
        : error?.name === 'NotSupportedError'
          ? 'This browser cannot record at 16 kHz. Try Chrome or Edge.'
          : (error?.message ?? String(error));
    say(message, true);
  } finally {
    state.starting = false;
    render();
  }
}

async function stop(message = 'Stopped. The microphone is released.', error = false) {
  const session = state.session;
  state.session = null;
  if (session) {
    cancelAnimationFrame(session.frame);
    await session.manager.close().catch(() => {});
    session.stream.getTracks().forEach((track) => track.stop());
    await session.context.close().catch(() => {});
  }
  ui.level.style.width = '0%';
  say(message, error);
  render();
}

async function restart() {
  await stop('Restarting with the new phrases…');
  await start();
}

function heard(event) {
  state.heard += 1;
  ui.heardCount.textContent = String(state.heard);
  const item = document.createElement('li');
  const name = document.createElement('span');
  name.textContent = event.keyword;
  const time = document.createElement('time');
  const now = new Date();
  time.dateTime = now.toISOString();
  time.textContent = now.toLocaleTimeString();
  item.append(name, time);
  ui.heard.prepend(item);
  while (ui.heard.children.length > MAX_HEARD) ui.heard.lastElementChild.remove();
  ui.burst.textContent = `“${event.keyword}”`;
  ui.burst.classList.remove('is-on');
  void ui.burst.offsetWidth; // restart the animation
  ui.burst.classList.add('is-on');
}

// ---------- Controls ----------
function describeSensitivity(value) {
  // Sensitivity 0 aims at 0.1 false alarms per hour of speech, 1 at 10, log-linear between.
  const rate = 0.1 * 100 ** value;
  const shown = rate < 1 ? rate.toFixed(1) : rate < 10 ? rate.toFixed(1).replace(/\.0$/, '') : rate.toFixed(0);
  return `Aims at about ${shown} false alarm${shown === '1' ? '' : 's'} per hour of speech.`;
}

ui.form.addEventListener('submit', (event) => {
  event.preventDefault();
  void addPhrase(ui.phrase.value);
});
ui.sensitivity.addEventListener('input', () => {
  state.sensitivity = Number(ui.sensitivity.value);
  ui.sensitivityValue.textContent = state.sensitivity.toFixed(2);
  ui.rate.textContent = describeSensitivity(state.sensitivity);
});
ui.sensitivity.addEventListener('change', () => {
  if (state.session) void restart();
});
ui.listen.addEventListener('click', () => void (state.session ? stop() : start()));
ui.download.addEventListener('click', () => {
  const blob = new Blob([JSON.stringify(keywordFile(), null, 2) + '\n'], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'keywords.json';
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  say('Downloaded keywords.json: use it with any Phonlet Wake SDK and the en-1 model.');
});
window.addEventListener('pagehide', () => void stop());

const fetchJson = async (url) => {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
};

try {
  const [config, file] = await Promise.all([fetchJson(CONFIG), fetchJson(KEYWORDS)]);
  // The keyword file names its model by SHA-256; the config names the model file the same way.
  if (config.sha256 !== file.model?.sha256) throw new Error('the phrases belong to another model');
  state.model = file.model;
  state.phrases = file.keywords.map((entry) => ({ entry, source: entry.metadata?.source }));
  say('Type any phrase and press Enter, or start listening for the phrases below.');
} catch (error) {
  say(`Could not load the phrases: ${error.message}`, true);
}
ui.rate.textContent = describeSensitivity(state.sensitivity);
render();
