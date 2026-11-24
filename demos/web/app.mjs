import {Wake, readWav} from './vendor/wake/index.mjs';
import {WakeManager} from './vendor/wake/capture.mjs';

const $ = id => document.getElementById(id);
let model, file, manager, busy = false, detections = 0;
const showError = error => { $('error').textContent = error.message; };

function showEvent(event) {
  $('empty')?.remove();
  const row = document.createElement('li');
  const heading = document.createElement('div');
  heading.className = 'event-heading';
  const name = document.createElement('strong');
  name.textContent = event.keyword;
  const time = document.createElement('time');
  time.textContent = `${event.timeSeconds.toFixed(3)} s`;
  const detail = document.createElement('span');
  detail.className = 'event-detail';
  detail.textContent = `Sensitivity ${event.sensitivity.toFixed(2)}`;
  heading.append(name, time);
  row.append(heading, detail);
  $('events').prepend(row);
  $('detection-count').textContent = String(++detections);
  while ($('events').children.length > 100) $('events').lastChild.remove();
}

function controls(active) {
  busy = active;
  $('start').disabled = active || !file;
  $('file').disabled = active || !file;
  $('stop').disabled = !active || !manager;
  for (const input of document.querySelectorAll('#keywords input, #sensitivity, #simd, #reset')) input.disabled = active;
}

function options() {
  if (!$('sensitivity').checkValidity()) throw new Error('Choose a default sensitivity between 0 and 1.');
  // The keyword file narrowed to the checked phrases, with any per-phrase sensitivity.
  const chosen = [...$('keywords').children].filter(row => row.querySelector('[type=checkbox]').checked);
  return {
    model, sensitivity: Number($('sensitivity').value), simd: $('simd').checked,
    keywords: {...file, keywords: chosen.map(row => {
      const phrase = file.keywords.find(kw => kw.name === row.dataset.id);
      return row.dataset.overridden ? {...phrase, sensitivity: Number(row.querySelector('[type=range]').value)} : phrase;
    })},
  };
}

function updateSelection() {
  const count = $('keywords').querySelectorAll('[type=checkbox]:checked').length;
  $('selected-count').textContent = `${count} selected`;
}

function updateDefaults(reset = false) {
  if (!$('sensitivity').checkValidity()) return;
  for (const row of $('keywords').children) {
    if (reset) delete row.dataset.overridden;
    if (!row.dataset.overridden) {
      row.querySelector('[type=range]').value = $('sensitivity').value;
      row.querySelector('output').textContent = Number($('sensitivity').value).toFixed(2);
    }
  }
}

try {
  [model, file] = await Promise.all([
    fetch('./assets/model.pwake').then(r => r.arrayBuffer()).then(b => new Uint8Array(b)),
    fetch('./assets/keywords.json').then(r => r.json()),
  ]);
  for (const kw of file.keywords) {
    const row = document.createElement('div');
    row.className = 'keyword';
    row.dataset.id = kw.name;
    const label = document.createElement('label');
    const check = document.createElement('input');
    check.type = 'checkbox';
    check.checked = kw.name === 'hey phonlet';
    const text = document.createElement('span');
    text.textContent = kw.name;
    label.append(check, text);
    check.onchange = updateSelection;
    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = 0; slider.max = 1; slider.step = .05; slider.value = .5;
    slider.setAttribute('aria-label', `${kw.name} sensitivity`);
    const output = document.createElement('output');
    output.textContent = '0.50';
    slider.oninput = () => { row.dataset.overridden = 'true'; output.textContent = Number(slider.value).toFixed(2); };
    row.append(label, slider, output);
    $('keywords').append(row);
  }
  $('status').textContent = 'Ready · model loaded locally';
  updateSelection();
  controls(false);
} catch (error) {
  showError(error);
  $('status').textContent = 'Could not load model';
}

$('sensitivity').oninput = () => updateDefaults();
$('reset').onclick = () => updateDefaults(true);
$('start').onclick = async () => {
  $('error').textContent = '';
  controls(true);
  $('status').textContent = 'Starting microphone…';
  try {
    manager = new WakeManager({wake: options(), onDetection: showEvent, onError: error => {
      showError(error);
      void stop();
    }});
    await manager.start();
    $('status').textContent = 'Listening locally';
    $('stop').disabled = false;
  } catch (error) {
    showError(error);
    await manager?.close();
    manager = null;
    controls(false);
    $('status').textContent = 'Ready · microphone not started';
  }
};

async function stop() {
  const active = manager;
  manager = null;
  $('stop').disabled = true;
  try { await active?.close(); }
  catch (error) { showError(error); }
  finally { controls(false); $('status').textContent = 'Stopped'; }
}
$('stop').onclick = stop;
$('file').onchange = async () => {
  const file = $('file').files[0];
  if (!file || busy) return;
  let wake;
  controls(true);
  $('error').textContent = '';
  try {
    const pcm = readWav(new Uint8Array(await file.arrayBuffer()));
    wake = await Wake.create(options());
    $('status').textContent = `Processing ${file.name}…`;
    for (let i = 0; i < pcm.length; i += 4096) {
      for (const event of await wake.process(pcm.subarray(i, i + 4096))) showEvent(event);
    }
    const result = await wake.finish();
    $('status').textContent = `Finished · ${result.frames} frames · ${result.discardedSamples} trailing samples discarded`;
  } catch (error) { showError(error); }
  finally {
    await wake?.close();
    controls(false);
    $('file').value = '';
  }
};
window.addEventListener('pagehide', () => { void manager?.close(); });
