// Lifecycle tests for the React adapter using an injected fake manager: the
// Web SDK itself is never loaded here (no Worker/fetch in Node). Install the
// dev dependencies first: bash sdk/react/test/install-deps.sh
import assert from 'node:assert/strict';
import test from 'node:test';

const jsdom = await import('jsdom').then(m => m, () => null);
if (!jsdom) {
  console.log('sdk/react tests: jsdom not installed (sdk/react/test/install-deps.sh) — skipping');
  process.exit(0);
}

const dom = new jsdom.JSDOM('<!doctype html><html><body><div id="root"></div></body></html>', {pretendToBeVisual: true});
for (const [name, value] of Object.entries({
  window: dom.window, document: dom.window.document, navigator: dom.window.navigator,
  HTMLElement: dom.window.HTMLElement,
})) {
  try { Object.defineProperty(globalThis, name, {value, configurable: true}); }
  catch { /* already defined by the Node runtime; jsdom's is only needed by React DOM */ }
}

const React = (await import('react')).default ?? (await import('react'));
const {createRoot} = await import('react-dom/client');
const act = (await import('react')).act ?? (await import('react-dom/test-utils')).act;
const {useWake} = await import('../index.mjs');

function makeFakeManager() {
  const created = [];
  class FakeManager {
    static created = created;
    constructor(options) { this.options = options; this.state = 'idle'; this.closed = false; created.push(this); }
    async start() { this.state = 'listening'; }
    async stop() { this.state = 'idle'; }
    async close() { this.state = 'closed'; this.closed = true; }
    detect(event) { this.options.onDetection(event); }
    fail(error) { this.options.onError(error); }
  }
  return {FakeManager, created};
}

async function mount(factory, extra = {}) {
  const root = createRoot(document.getElementById('root'));
  let api;
  await act(async () => {
    root.render(React.createElement(function HookHost() {
      api = useWake({model: 'model.pwake', keywords: 'keywords.json', factory, ...extra});
      return null;
    }));
  });
  return {root, api: () => api};
}

test('start reaches listening and keeps exactly one manager', async () => {
  const {FakeManager, created} = makeFakeManager();
  const {root, api} = await mount(async () => FakeManager);
  assert.equal(api().state, 'idle');
  await act(async () => { await api().start(); });
  assert.equal(api().state, 'listening');
  await act(async () => { await api().start(); }); // repeated start is idempotent
  assert.equal(created.length, 1);
  await act(async () => { root.unmount(); });
});

test('stop returns to idle; restart reuses the manager (fresh stream state)', async () => {
  const {FakeManager, created} = makeFakeManager();
  const {root, api} = await mount(async () => FakeManager);
  await act(async () => { await api().start(); });
  await act(async () => { await api().stop(); });
  assert.equal(api().state, 'idle');
  await act(async () => { await api().restart(); });
  assert.equal(api().state, 'listening');
  assert.equal(created.length, 1);
  await act(async () => { root.unmount(); });
});

test('unmount while listening closes the manager', async () => {
  const {FakeManager, created} = makeFakeManager();
  const {root, api} = await mount(async () => FakeManager);
  await act(async () => { await api().start(); });
  await act(async () => { root.unmount(); });
  assert.equal(created.length, 1);
  assert.equal(created[0].closed, true);
});

test('detections reach the callback; manager errors surface as error state', async () => {
  const {FakeManager, created} = makeFakeManager();
  const detections = [];
  const {root, api} = await mount(async () => FakeManager, {onDetection: event => detections.push(event)});
  await act(async () => { await api().start(); });
  await act(async () => { created[0].detect({keywordId: 'hey phonlet', frameIndex: 3}); });
  assert.deepEqual(detections, [{keywordId: 'hey phonlet', frameIndex: 3}]);
  const failure = new Error('microphone denied');
  await act(async () => { created[0].fail(failure); });
  assert.equal(api().state, 'error');
  assert.equal(api().error, failure);
  await act(async () => { root.unmount(); });
});

test('late async completion after unmount does not touch state', async () => {
  const {FakeManager, created} = makeFakeManager();
  const {root, api} = await mount(async () => FakeManager);
  await act(async () => { await api().start(); });
  await act(async () => { root.unmount(); });
  await act(async () => { created[0].fail(new Error('late')); });
  assert.equal(api().state, 'listening'); // frozen at the last pre-unmount value
});
