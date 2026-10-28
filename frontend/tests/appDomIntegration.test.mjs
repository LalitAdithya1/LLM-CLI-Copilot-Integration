import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const appSource = readFileSync(new URL('../js/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const apiSource = readFileSync(new URL('../js/api.js', import.meta.url), 'utf8');
const workflowSource = readFileSync(new URL('../js/proposalWorkflow.js', import.meta.url), 'utf8');
const diffViewSource = readFileSync(new URL('../js/diffView.js', import.meta.url), 'utf8');
const chatSource = readFileSync(new URL('../js/chatTranscript.js', import.meta.url), 'utf8');

function dataUrl(source, suffix) {
  return `data:text/javascript;base64,${Buffer.from(source).toString('base64')}#${suffix}`;
}

class FakeElement {
  constructor(selector = '') {
    this.selector = selector;
    this.listeners = new Map();
    this.attributes = new Map();
    this.disabled = false;
    this.hidden = false;
    this.value = '';
    this.textContent = '';
    this.scrollTop = 0;
    this.dataset = {};
  }

  addEventListener(type, callback) {
    const handlers = this.listeners.get(type) || [];
    handlers.push(callback);
    this.listeners.set(type, handlers);
  }

  dispatch(type, event = {}) {
    if (this.disabled) return;
    for (const callback of this.listeners.get(type) || []) callback({ preventDefault() {}, ...event });
  }

  setAttribute(name, value) { this.attributes.set(name, value); }
  removeAttribute(name) { this.attributes.delete(name); }
  focus() { this.focused = true; }
  replaceChildren(...nodes) {
    this.children = nodes.length === 1 && Array.isArray(nodes[0]?.children) && nodes[0].selector === undefined
      ? nodes[0].children
      : nodes;
  }
  append(...nodes) { this.children = [...(this.children || []), ...nodes]; }
}

// Concatenated text of an element and its descendants.
function textOf(node) {
  return [node.textContent || '', ...(node.children || []).map(textOf)].join(' ');
}

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

async function createApp(fetchOverride = null) {
  const elements = new Map();
  const appRequiredSelectors = [...appSource.matchAll(/'([^']+)'/g)]
    .map((match) => match[1])
    .filter((selector) => selector.startsWith('#') || selector.startsWith('[data-app'));
  for (const selector of appRequiredSelectors) elements.set(selector, new FakeElement(selector));
  const modeButtons = ['generate', 'refactor', 'files'].map((mode) => {
    const button = new FakeElement(`mode-${mode}`);
    button.dataset.mode = mode;
    return button;
  });
  const document = {
    querySelector(selector) { return elements.get(selector) || null; },
    querySelectorAll(selector) { return selector === '.mode-card[data-mode]' ? modeButtons : []; },
    createDocumentFragment() { return { children: [], append(element) { this.children.push(element); } }; },
    createElement(tag) { return new FakeElement(tag); },
    createTextNode(text) { return { textContent: text }; },
  };
  const requests = [];
  // record is false when an override falls back here; it already recorded.
  const defaultFetch = async (url, options, record = true) => {
    const path = new URL(url).pathname;
    const body = options.body ? JSON.parse(options.body) : null;
    if (record) requests.push({ path, method: options.method, body });
    if (path === '/api/health') return jsonResponse({ status: 'ok' });
    if (path === '/api/config') return jsonResponse({
      supportedExtensions: ['.cpp'],
      launchDirectory: '/tmp',
      activeOutputDirectory: '/tmp',
    });
    if (path === '/api/refactor') return jsonResponse({
      proposalId: 'prop-1', status: 'PENDING', originalCode: body.sourceCode,
      proposedCode: 'refactored <script>unsafe()</script>', baseEditorHash: 'fnv1a64:0123456789abcdef',
    });
    if (path === '/api/diff') return jsonResponse({ diff: '--- old\n+++ new\n' });
    if (path.endsWith('/approve')) return jsonResponse({ proposalId: 'prop-1', status: 'APPROVED' });
    if (path.endsWith('/reject')) return jsonResponse({ proposalId: 'prop-1', status: 'REJECTED' });
    if (path.endsWith('/mark-applied')) return jsonResponse({ proposalId: 'prop-1', status: 'APPLIED' });
    if (path === '/api/save') return jsonResponse({ savedPath: '/tmp/main.cpp', status: 'SAVED' });
    if (path === '/api/generate') return jsonResponse({ generatedCode: '<img src=x onerror=alert(1)>', language: body.language });
    throw new Error(`Unexpected endpoint ${path}`);
  };

  globalThis.document = document;
  globalThis.window = { confirm: () => true };
  globalThis.fetch = fetchOverride
    ? (url, options) => {
      const path = new URL(url).pathname;
      const body = options.body ? JSON.parse(options.body) : null;
      requests.push({ path, method: options.method, body });
      return fetchOverride(path, body, (fallbackUrl, fallbackOptions) => defaultFetch(fallbackUrl, fallbackOptions, false));
    }
    : defaultFetch;
  const apiUrl = dataUrl(apiSource, Math.random());
  const workflowUrl = dataUrl(workflowSource, Math.random());
  const rewrittenApp = appSource
    .replace("from './api.js'", `from '${apiUrl}'`)
    .replace("from './proposalWorkflow.js'", `from '${workflowUrl}'`)
    .replace("from './diffView.js'", `from '${dataUrl(diffViewSource, Math.random())}'`)
    .replace("from './chatTranscript.js'", `from '${dataUrl(chatSource, Math.random())}'`);
  await import(dataUrl(rewrittenApp, Math.random()));
  await new Promise((resolve) => setTimeout(resolve, 0));
  return { elements, modeButtons, requests };
}

async function createPendingProposal(app) {
  const { elements, modeButtons } = app;
  modeButtons.find((button) => button.dataset.mode === 'refactor').dispatch('click');
  elements.get('#prompt-input').value = 'Improve names';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(elements.get('#proposal-review-status').textContent, 'Proposal pending review');
}

test('HTML provides every required app selector and proposal control', () => {
  const ids = new Set([...htmlSource.matchAll(/\bid="([^"]+)"/g)].map((match) => match[1]));
  const selectors = [...appSource.matchAll(/'(#[-\w]+|\[data-app="[^"]+"\])'/g)].map((match) => match[1]);
  for (const selector of selectors) {
    if (selector.startsWith('#')) assert.ok(ids.has(selector.slice(1)), `missing ${selector} in index.html`);
    else assert.match(htmlSource, /data-app="codeagent"/);
  }
  for (const id of ['review-changes-button', 'accept-proposal-button', 'reject-proposal-button', 'save-file-button']) {
    assert.ok(ids.has(id));
  }
});

test('actual review, approve, and save handlers gate state and suppress duplicate clicks', async () => {
  const diff = deferred();
  const approval = deferred();
  const save = deferred();
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return diff.promise;
    if (path.endsWith('/approve')) return approval.promise;
    if (path === '/api/save') return save.promise;
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(app);
  const { elements, requests } = app;
  const reviewButton = elements.get('#review-changes-button');
  const approveButton = elements.get('#accept-proposal-button');
  const saveButton = elements.get('#save-file-button');
  assert.equal(approveButton.disabled, true);
  assert.equal(saveButton.disabled, true);

  reviewButton.dispatch('click');
  assert.equal(reviewButton.disabled, true);
  assert.match(reviewButton.textContent, /Loading diff/);
  reviewButton.dispatch('click');
  assert.equal(requests.filter((request) => request.path === '/api/diff').length, 1);
  diff.resolve(jsonResponse({ diff: 'reviewed diff' }));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(approveButton.disabled, false);
  assert.deepEqual(requests.find((request) => request.path === '/api/diff').body, { proposalId: 'prop-1' });

  approveButton.dispatch('click');
  assert.equal(approveButton.disabled, true);
  approveButton.dispatch('click');
  assert.equal(requests.filter((request) => request.path.endsWith('/approve')).length, 1);
  approval.resolve(jsonResponse({ proposalId: 'prop-1', status: 'APPROVED' }));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.deepEqual(requests.find((request) => request.path.endsWith('/approve')).body, null);
  // Accept puts the proposal into the editor; nothing is written and the
  // proposal is not routed through Save output.
  assert.equal(elements.get('#code-editor').value, 'refactored <script>unsafe()</script>');
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes');
  assert.match(elements.get('#proposal-review-status').textContent, /Accepted into the editor · not saved/);
  assert.equal(saveButton.disabled, true);
  assert.equal(requests.filter((request) => request.path === '/api/save').length, 0);

  globalThis.window.prompt = () => 'main.cpp';
  const saveAsButton = elements.get('#save-document-as-button');
  saveAsButton.dispatch('click');
  assert.equal(saveAsButton.disabled, true);
  saveAsButton.dispatch('click');
  assert.equal(requests.filter((request) => request.path === '/api/save').length, 1);
  assert.deepEqual(requests.find((request) => request.path === '/api/save').body, {
    fileName: 'main.cpp', content: 'refactored <script>unsafe()</script>', overwriteConfirmed: false,
  });
  save.resolve(jsonResponse({ savedPath: '/tmp/main.cpp', status: 'SAVED' }));
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.match(elements.get('#document-status').textContent, /Saved \/tmp\/main\.cpp/);
  assert.equal(elements.get('#code-status').textContent, 'Not saved to disk',
    'without a re-read the saved copy is not treated as an in-place file');
});

test('failed decisions remain pending, surface backend errors, and do not enable save', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return jsonResponse({ diff: 'diff' });
    if (path.endsWith('/approve')) return jsonResponse({
      error: { code: 'INTERNAL_ERROR', message: 'The API request could not be completed.' },
    }, 500);
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(app);
  app.elements.get('#review-changes-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  app.elements.get('#accept-proposal-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get('#proposal-review-status').textContent, 'Proposal pending review');
  assert.equal(app.elements.get('#save-file-button').disabled, true);
  assert.equal(app.elements.get('#proposal-review-availability').textContent, 'The API request could not be completed.');
});

test('actual reject control sends one request and reports only the confirmed result', async () => {
  const rejection = deferred();
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return jsonResponse({ diff: 'diff' });
    if (path.endsWith('/reject')) return rejection.promise;
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(app);
  app.elements.get('#review-changes-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  const rejectButton = app.elements.get('#reject-proposal-button');
  rejectButton.dispatch('click');
  assert.equal(rejectButton.disabled, true);
  rejectButton.dispatch('click');
  assert.equal(app.requests.filter((request) => request.path.endsWith('/reject')).length, 1);
  assert.equal(app.elements.get('#proposal-review-status').textContent, 'Waiting for rejection confirmation…');
  rejection.resolve(jsonResponse({ proposalId: 'prop-1', status: 'REJECTED' }));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get('#proposal-review-status').textContent, 'Rejected');
  assert.match(app.elements.get('#notification-area').textContent, /rejected by the backend/);
  assert.equal(app.elements.get('#save-file-button').disabled, true);
});

test('a failed save after Accept keeps the accepted code in the editor as unsaved', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return jsonResponse({ diff: 'diff' });
    if (path.endsWith('/approve')) return jsonResponse({ proposalId: 'prop-1', status: 'APPROVED' });
    if (path === '/api/save') return jsonResponse({
      error: { code: 'SAVE_FAILED', message: 'File could not be saved.' },
    }, 500);
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(app);
  app.elements.get('#review-changes-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  app.elements.get('#accept-proposal-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  globalThis.window.prompt = () => 'main.cpp';
  app.elements.get('#save-document-as-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.match(app.elements.get('#document-status').textContent, /Save As failed: File could not be saved/);
  assert.equal(app.elements.get('#code-editor').value, 'refactored <script>unsafe()</script>');
  assert.equal(app.elements.get('#code-status').textContent, 'Unsaved changes');
  assert.doesNotMatch(app.elements.get('#notification-area').textContent, /Saved \//);
});

test('stale editor content blocks approval and save; rejected API state is confirmed only on success', async () => {
  let approvalCalls = 0;
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return jsonResponse({ diff: 'diff' });
    if (path.endsWith('/approve')) {
      approvalCalls += 1;
      return jsonResponse({ proposalId: 'prop-1', status: 'APPROVED' });
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(app);
  app.elements.get('#review-changes-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  const editor = app.elements.get('#code-editor');
  editor.value += '// changed';
  editor.dispatch('input');
  assert.equal(app.elements.get('#accept-proposal-button').disabled, true);
  app.elements.get('#accept-proposal-button').dispatch('click');
  assert.equal(approvalCalls, 0);
  assert.equal(app.elements.get('#save-file-button').disabled, true);
  assert.equal(app.requests.filter((request) => request.path === '/api/save').length, 0);

  const approvedApp = await createApp((path, body, fallback) => {
    if (path === '/api/diff') return jsonResponse({ diff: 'diff' });
    if (path.endsWith('/approve')) return jsonResponse({ proposalId: 'prop-1', status: 'APPROVED' });
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  await createPendingProposal(approvedApp);
  approvedApp.elements.get('#review-changes-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  approvedApp.elements.get('#accept-proposal-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  const approvedEditor = approvedApp.elements.get('#code-editor');
  approvedEditor.value += '// stale';
  approvedEditor.dispatch('input');
  assert.equal(approvedApp.elements.get('#save-file-button').disabled, true);
  approvedApp.elements.get('#save-file-name').value = 'main.cpp';
  approvedApp.elements.get('#save-file-button').dispatch('click');
  assert.equal(approvedApp.requests.filter((request) => request.path === '/api/save').length, 0);
});

test('generated code and API errors are assigned as text content', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({ generatedCode: '<img src=x onerror=alert(1)>', language: body.language });
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  app.elements.get('#prompt-input').value = 'Generate it';
  app.elements.get('#language-input').value = 'C++';
  app.elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get('#proposal-proposed-code').textContent, '<img src=x onerror=alert(1)>');

  const failing = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({
      error: { code: 'GENERATION_FAILED', message: '<script>bad</script>' },
    }, 502);
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  failing.elements.get('#prompt-input').value = 'Generate it';
  failing.elements.get('#language-input').value = 'C++';
  failing.elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.match(failing.elements.get('#notification-area').textContent, /<script>bad<\/script>/);
});

test('generated code saves once and replaces an existing file only after confirmation', async () => {
  const saveBodies = [];
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({ generatedCode: 'int main() {}\n', language: body.language });
    if (path === '/api/save') {
      saveBodies.push(body);
      return body.overwriteConfirmed
        ? jsonResponse({ savedPath: '/tmp/gen.cpp', status: 'SAVED' })
        : jsonResponse({ error: { code: 'OVERWRITE_CONFIRMATION_REQUIRED', message: 'exists' } }, 409);
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  const { elements } = app;
  const saveButton = elements.get('#save-file-button');
  assert.equal(saveButton.disabled, true);
  elements.get('#prompt-input').value = 'Write main';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(saveButton.disabled, false);

  let confirmations = 0;
  globalThis.window.confirm = () => { confirmations += 1; return true; };
  elements.get('#save-file-name').value = 'gen.cpp';
  saveButton.dispatch('click');
  saveButton.dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(confirmations, 1);
  assert.deepEqual(saveBodies, [
    { fileName: 'gen.cpp', content: 'int main() {}\n', overwriteConfirmed: false },
    { fileName: 'gen.cpp', content: 'int main() {}\n', overwriteConfirmed: true },
  ]);
  assert.match(elements.get('#notification-area').textContent, /confirmed the save: \/tmp\/gen\.cpp/);
  assert.equal(saveButton.disabled, true);
});

test('declined overwrite of generated code does not send a confirmed save', async () => {
  const saveBodies = [];
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({ generatedCode: 'int x;\n', language: body.language });
    if (path === '/api/save') {
      saveBodies.push(body);
      return jsonResponse({ error: { code: 'OVERWRITE_CONFIRMATION_REQUIRED', message: 'exists' } }, 409);
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  globalThis.window.confirm = () => false;
  app.elements.get('#prompt-input').value = 'Write x';
  app.elements.get('#language-input').value = 'C++';
  app.elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  app.elements.get('#save-file-name').value = 'x.cpp';
  app.elements.get('#save-file-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(saveBodies.length, 1);
  assert.equal(saveBodies[0].overwriteConfirmed, false);
  assert.match(app.elements.get('#notification-area').textContent, /not confirmed/);
  assert.equal(app.elements.get('#save-file-button').disabled, false);
});

test('opening a local file loads it into the editor and refactors that content', async () => {
  const app = await createApp();
  const { elements, modeButtons, requests } = app;
  const input = elements.get('#load-source-file-input');
  const source = 'int add(int a,int b){return a+b;}\n';
  input.files = [{ name: 'math.hpp', size: source.length, text: async () => source }];
  input.dispatch('change');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(elements.get('#code-editor').value, source);
  assert.equal(elements.get('#preview-heading').textContent, 'math.hpp');
  assert.equal(elements.get('#save-file-name').value, 'math.hpp');
  assert.equal(elements.get('#code-status').textContent, 'Not saved to disk');
  assert.match(elements.get('#code-language').textContent, /location unknown/);
  assert.equal(elements.get('#save-document-button').disabled, true);

  input.files = [{ name: 'notes.txt', size: 3, text: async () => 'abc' }];
  input.dispatch('change');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(elements.get('#code-editor').value, source);
  assert.match(elements.get('#notification-area').textContent, /not a supported source file/);

  modeButtons.find((button) => button.dataset.mode === 'refactor').dispatch('click');
  elements.get('#prompt-input').value = 'Add spaces';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(requests.find((request) => request.path === '/api/refactor').body.sourceCode, source);
});

test('provider failures point the user to the terminal for the cause', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({
      error: { code: 'GENERATION_FAILED', message: 'Code generation failed.' },
    }, 502);
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  app.elements.get('#prompt-input').value = 'Generate it';
  app.elements.get('#language-input').value = 'C++';
  app.elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.match(app.elements.get('#notification-area').textContent, /terminal running codeagent.*GEMINI_API_KEY/);
});

const settle = () => new Promise((resolve) => setTimeout(resolve, 10));
const REVISION_A = 'fnv1a64:aaaaaaaaaaaaaaaa';
const REVISION_B = 'fnv1a64:bbbbbbbbbbbbbbbb';
const REVISION_C = 'fnv1a64:cccccccccccccccc';

function typeInEditor(elements, text) {
  const editor = elements.get('#code-editor');
  editor.value = text;
  editor.dispatch('input');
}

function documentBackend(overrides = {}) {
  const writes = [];
  const reads = [];
  let revision = REVISION_A;
  const handler = (path, body, fallback) => {
    if (overrides[path]) {
      const result = overrides[path](body);
      if (result) return result;
    }
    if (path === '/api/files/open-dialog') {
      return jsonResponse({ status: 'SELECTED', selectedPath: '/work/insertion_sort.cpp' });
    }
    if (path === '/api/files/read') {
      reads.push(body.path);
      return jsonResponse({ path: body.path, content: `// ${body.path}\n`, revision: REVISION_A });
    }
    if (path === '/api/files/write') {
      writes.push(body);
      revision = revision === REVISION_A ? REVISION_B : REVISION_C;
      return jsonResponse({ savedPath: body.path, status: 'SAVED', revision });
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  };
  return { handler, writes, reads };
}

test('a new document is untitled.cpp and the unsaved marker follows edits', async () => {
  const { elements } = await createApp();
  assert.equal(elements.get('#preview-heading').textContent, 'untitled.cpp');
  assert.equal(elements.get('#code-status').textContent, 'Not saved to disk');
  assert.match(elements.get('#code-language').textContent, /New document/);
  assert.equal(elements.get('#save-document-button').disabled, true);
  assert.equal(elements.get('#save-document-as-button').disabled, false);
  assert.equal(elements.get('#reload-document-button').disabled, true);

  typeInEditor(elements, 'int changed;\n');
  assert.equal(elements.get('#preview-heading').textContent, 'untitled.cpp ●');
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes');
  assert.equal(elements.get('#save-document-button').disabled, true, 'no file to save in place');
});

test('Choose source path opens the file, and Save writes the edits back to that path', async () => {
  const backend = documentBackend();
  const { elements } = await createApp(backend.handler);
  elements.get('#select-source-file-button').dispatch('click');
  await settle();
  assert.deepEqual(backend.reads, ['/work/insertion_sort.cpp']);
  assert.equal(elements.get('#code-editor').value, '// /work/insertion_sort.cpp\n');
  assert.equal(elements.get('#preview-heading').textContent, 'insertion_sort.cpp');
  assert.equal(elements.get('#code-language').textContent, '/work/insertion_sort.cpp');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
  assert.equal(elements.get('#source-path-input').value, '/work/insertion_sort.cpp');
  assert.equal(elements.get('#save-document-button').disabled, true, 'nothing to save yet');

  typeInEditor(elements, 'int sorted;\n');
  assert.equal(elements.get('#preview-heading').textContent, 'insertion_sort.cpp ●');
  const saveButton = elements.get('#save-document-button');
  assert.equal(saveButton.disabled, false);
  saveButton.dispatch('click');
  saveButton.dispatch('click');
  await settle();
  assert.deepEqual(backend.writes, [{
    path: '/work/insertion_sort.cpp', content: 'int sorted;\n', baseRevision: REVISION_A, overwriteConfirmed: false,
  }]);
  assert.equal(elements.get('#preview-heading').textContent, 'insertion_sort.cpp');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
  assert.match(elements.get('#document-status').textContent, /Saved \/work\/insertion_sort\.cpp/);
  assert.equal(saveButton.disabled, true);

  typeInEditor(elements, 'int sortedAgain;\n');
  elements.get('#code-editor').dispatch('keydown', { key: 's', ctrlKey: true });
  await settle();
  assert.equal(backend.writes[1].baseRevision, REVISION_B, 'the next save is based on the saved revision');
});

test('without a native dialog, the source path can be typed and errors stay visible', async () => {
  const backend = documentBackend({
    '/api/files/open-dialog': () => jsonResponse({
      error: { code: 'DIALOG_UNAVAILABLE', message: 'Native file dialog is unavailable.' },
    }, 503),
    '/api/files/read': (body) => (body.path.endsWith('missing.cpp') ? jsonResponse({
      error: { code: 'INVALID_SOURCE_FILE', message: 'Path is not an existing file with a supported source extension.' },
    }, 422) : null),
  });
  const { elements } = await createApp(backend.handler);
  const fileStatus = elements.get('#file-status');
  elements.get('#select-source-file-button').dispatch('click');
  await settle();
  assert.equal(fileStatus.hidden, false);
  assert.equal(fileStatus.dataset.tone, 'error');
  assert.match(fileStatus.textContent, /No native file dialog.*Open path/);
  assert.equal(elements.get('#source-path-input').focused, true);

  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  assert.equal(elements.get('#source-path-input').attributes.get('aria-invalid'), 'true');
  assert.deepEqual(backend.reads, []);

  elements.get('#source-path-input').value = '/work/missing.cpp';
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  assert.match(fileStatus.textContent, /Could not open the file: Path is not an existing file/);
  assert.equal(elements.get('#preview-heading').textContent, 'untitled.cpp');

  elements.get('#source-path-input').value = 'D:\\work\\main.cpp';
  elements.get('#source-path-input').dispatch('keydown', { key: 'Enter' });
  await settle();
  assert.deepEqual(backend.reads, ['D:\\work\\main.cpp'], 'the failed read was answered by the override');
  assert.equal(elements.get('#preview-heading').textContent, 'main.cpp');
  assert.equal(fileStatus.dataset.tone, 'success');
});

test('saving over a file changed on disk requires confirmation', async () => {
  let rejectNextWrite = true;
  const backend = documentBackend({
    '/api/files/write': (body) => {
      if (!body.overwriteConfirmed && rejectNextWrite) {
        rejectNextWrite = false;
        return jsonResponse({ error: { code: 'FILE_CHANGED_ON_DISK', message: 'The file changed on disk.' } }, 409);
      }
      return null;
    },
  });
  const { elements } = await createApp(backend.handler);
  elements.get('#source-path-input').value = '/work/a.cpp';
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  typeInEditor(elements, 'int mine;\n');

  globalThis.window.confirm = () => false;
  elements.get('#save-document-button').dispatch('click');
  await settle();
  assert.equal(backend.writes.length, 0);
  assert.match(elements.get('#document-status').textContent, /Save cancelled.*Reload/);
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes');

  rejectNextWrite = true;
  let question = '';
  globalThis.window.confirm = (message) => { question = message; return true; };
  elements.get('#save-document-button').dispatch('click');
  await settle();
  assert.match(question, /changed on disk.*VS Code/s);
  assert.deepEqual(backend.writes.map((write) => write.overwriteConfirmed), [true]);
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
});

test('opening or reloading over unsaved edits asks first and keeps the edits when declined', async () => {
  const backend = documentBackend();
  const { elements } = await createApp(backend.handler);
  elements.get('#source-path-input').value = '/work/a.cpp';
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  typeInEditor(elements, 'int unsaved;\n');

  let question = '';
  globalThis.window.confirm = (message) => { question = message; return false; };
  elements.get('#source-path-input').value = '/work/b.cpp';
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  assert.match(question, /a\.cpp has unsaved changes/);
  assert.deepEqual(backend.reads, ['/work/a.cpp']);
  assert.equal(elements.get('#code-editor').value, 'int unsaved;\n');

  elements.get('#reload-document-button').dispatch('click');
  await settle();
  assert.deepEqual(backend.reads, ['/work/a.cpp']);

  const input = elements.get('#load-source-file-input');
  input.files = [{ name: 'c.cpp', size: 3, text: async () => 'int c;' }];
  input.dispatch('change');
  await settle();
  assert.equal(elements.get('#code-editor').value, 'int unsaved;\n');

  globalThis.window.confirm = () => true;
  elements.get('#reload-document-button').dispatch('click');
  await settle();
  assert.equal(elements.get('#code-editor').value, '// /work/a.cpp\n');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
});

test('Save As writes a new filename to the output directory and later saves go to it', async () => {
  const saves = [];
  const backend = documentBackend({
    '/api/save': (body) => {
      saves.push(body);
      if (!body.overwriteConfirmed) {
        return jsonResponse({ error: { code: 'OVERWRITE_CONFIRMATION_REQUIRED', message: 'exists' } }, 409);
      }
      return jsonResponse({ savedPath: `/tmp/${body.fileName}`, status: 'SAVED' });
    },
  });
  const { elements } = await createApp(backend.handler);
  typeInEditor(elements, 'int draft;\n');
  let promptText = '';
  globalThis.window.prompt = (message, defaultValue) => {
    promptText = `${message}|${defaultValue}`;
    return 'sorted.cpp';
  };
  let confirms = 0;
  globalThis.window.confirm = () => { confirms += 1; return true; };
  elements.get('#save-document-as-button').dispatch('click');
  await settle();
  assert.match(promptText, /active output directory:\n\/tmp\|untitled\.cpp$/);
  assert.equal(confirms, 1, 'replacing an existing file is confirmed');
  assert.deepEqual(saves, [
    { fileName: 'sorted.cpp', content: 'int draft;\n', overwriteConfirmed: false },
    { fileName: 'sorted.cpp', content: 'int draft;\n', overwriteConfirmed: true },
  ]);
  assert.deepEqual(backend.reads, ['/tmp/sorted.cpp']);
  assert.equal(elements.get('#preview-heading').textContent, 'sorted.cpp');
  assert.equal(elements.get('#code-language').textContent, '/tmp/sorted.cpp');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');

  typeInEditor(elements, 'int draft2;\n');
  elements.get('#save-document-button').dispatch('click');
  await settle();
  assert.equal(backend.writes[0].path, '/tmp/sorted.cpp');

  globalThis.window.prompt = () => null;
  elements.get('#save-document-as-button').dispatch('click');
  await settle();
  assert.equal(saves.length, 2);
  assert.match(elements.get('#document-status').textContent, /Save As cancelled/);
});

test('output directory can be typed, is validated by the backend, and is displayed', async () => {
  const directoryRequests = [];
  const backend = documentBackend({
    '/api/output/select-directory': (body) => {
      directoryRequests.push(body);
      if (!body.directoryPath) {
        return jsonResponse({ error: { code: 'DIALOG_UNAVAILABLE', message: 'Native directory dialog is unavailable.' } }, 503);
      }
      if (body.directoryPath === '/nope') {
        return jsonResponse({ error: { code: 'INVALID_OUTPUT_DIRECTORY', message: 'Selected path is not an existing directory.' } }, 422);
      }
      return jsonResponse({ status: 'SELECTED', activeOutputDirectory: '/mnt/d/out' });
    },
  });
  const { elements } = await createApp(backend.handler);
  const fileStatus = elements.get('#file-status');
  const input = elements.get('#output-directory-input');
  assert.equal(input.value, '/tmp', 'prefilled with the active directory');

  elements.get('#select-output-directory-button').dispatch('click');
  await settle();
  assert.match(fileStatus.textContent, /No native directory dialog.*Set directory/);
  assert.equal(input.focused, true);

  input.value = '/nope';
  elements.get('#set-output-directory-button').dispatch('click');
  await settle();
  assert.equal(fileStatus.dataset.tone, 'error');
  assert.match(fileStatus.textContent, /INVALID_OUTPUT_DIRECTORY.*not an existing directory/);
  assert.equal(input.attributes.get('aria-invalid'), 'true');
  assert.equal(elements.get('#backend-config-output-directory').textContent, '/tmp');

  input.value = 'D:\\out';
  input.dispatch('keydown', { key: 'Enter' });
  await settle();
  assert.deepEqual(directoryRequests.at(-1), { directoryPath: 'D:\\out' });
  assert.equal(elements.get('#backend-config-output-directory').textContent, '/mnt/d/out');
  assert.equal(input.value, '/mnt/d/out');
  assert.match(fileStatus.textContent, /Output directory set to \/mnt\/d\/out/);
});

async function openReviewedProposal(app, path = '/work/a.cpp') {
  const { elements, modeButtons } = app;
  elements.get('#source-path-input').value = path;
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  modeButtons.find((button) => button.dataset.mode === 'refactor').dispatch('click');
  elements.get('#prompt-input').value = 'Improve names';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await settle();
  elements.get('#review-changes-button').dispatch('click');
  await settle();
}

const ACCEPT_DIFF = '--- original\n+++ proposed\n@@ -1,1 +1,1 @@\n-// /work/a.cpp\n+refactored\n';

test('the reviewed diff is rendered as Git-style colored lines', async () => {
  const backend = documentBackend({ '/api/diff': () => jsonResponse({ diff: ACCEPT_DIFF }) });
  const app = await createApp(backend.handler);
  await openReviewedProposal(app);
  const spans = app.elements.get('#proposal-diff-content').children.filter((node) => node.className);
  assert.deepEqual(spans.map((span) => span.className.replace('diff-line diff-line--', '')),
    ['file-header', 'file-header', 'hunk', 'removed', 'added']);
  assert.deepEqual(spans.map((span) => span.textContent),
    ['--- original', '+++ proposed', '@@ -1,1 +1,1 @@', '-// /work/a.cpp', '+refactored']);
});

test('Accept puts the proposal into the editor unsaved, and Save writes the edited result', async () => {
  const backend = documentBackend({ '/api/diff': () => jsonResponse({ diff: ACCEPT_DIFF }) });
  const app = await createApp(backend.handler);
  const { elements, requests } = app;
  await openReviewedProposal(app);
  let confirms = 0;
  globalThis.window.confirm = () => { confirms += 1; return true; };
  const acceptButton = elements.get('#accept-proposal-button');
  acceptButton.dispatch('click');
  acceptButton.dispatch('click');
  await settle();
  assert.equal(confirms, 0, 'a clean editor needs no confirmation');
  assert.equal(requests.filter((request) => request.path.endsWith('/approve')).length, 1);
  assert.equal(elements.get('#code-editor').value, 'refactored <script>unsafe()</script>');
  assert.equal(elements.get('#preview-heading').textContent, 'a.cpp ●');
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes');
  assert.equal(elements.get('#save-document-button').disabled, false);
  assert.equal(acceptButton.disabled, true, 'an accepted proposal cannot be applied again');
  assert.equal(elements.get('#save-file-button').disabled, true, 'not routed through Save output');
  assert.equal(backend.writes.length, 0, 'Accept does not write to disk');
  assert.equal(requests.filter((request) => request.path === '/api/save').length, 0);

  typeInEditor(elements, 'refactored and hand-edited\n');
  elements.get('#save-document-button').dispatch('click');
  await settle();
  assert.deepEqual(backend.writes, [{
    path: '/work/a.cpp', content: 'refactored and hand-edited\n', baseRevision: REVISION_A, overwriteConfirmed: false,
  }]);
  assert.equal(elements.get('#preview-heading').textContent, 'a.cpp');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
});

test('Accept over unsaved editor changes asks first and does nothing when declined', async () => {
  const backend = documentBackend();
  const app = await createApp(backend.handler);
  const { elements, requests } = app;
  elements.get('#source-path-input').value = '/work/a.cpp';
  elements.get('#open-source-path-button').dispatch('click');
  await settle();
  typeInEditor(elements, 'int unsavedWork;\n');
  app.modeButtons.find((button) => button.dataset.mode === 'refactor').dispatch('click');
  elements.get('#prompt-input').value = 'Improve';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await settle();
  elements.get('#review-changes-button').dispatch('click');
  await settle();

  let question = '';
  globalThis.window.confirm = (message) => { question = message; return false; };
  elements.get('#accept-proposal-button').dispatch('click');
  await settle();
  assert.match(question, /a\.cpp has unsaved changes\. Replace the editor contents/);
  assert.equal(requests.filter((request) => request.path.endsWith('/approve')).length, 0);
  assert.equal(elements.get('#code-editor').value, 'int unsavedWork;\n');
  assert.equal(elements.get('#proposal-review-status').textContent, 'Proposal pending review');

  globalThis.window.confirm = () => true;
  elements.get('#accept-proposal-button').dispatch('click');
  await settle();
  assert.equal(elements.get('#code-editor').value, 'refactored <script>unsafe()</script>');
  assert.equal(backend.writes.length, 0);
});

test('mark-applied is sent once, only after the proposal is in the editor', async () => {
  let editorAtMarkTime = null;
  let app;
  const backend = documentBackend({
    '/api/changes/prop-1/mark-applied': () => {
      editorAtMarkTime = app.elements.get('#code-editor').value;
      return null;
    },
  });
  app = await createApp(backend.handler);
  const { elements, requests } = app;
  await openReviewedProposal(app);
  const accept = elements.get('#accept-proposal-button');
  accept.dispatch('click');
  accept.dispatch('click');
  await settle();
  accept.dispatch('click');
  await settle();
  const paths = requests.map((request) => request.path).filter((path) => path.startsWith('/api/changes/'));
  assert.deepEqual(paths, ['/api/changes/prop-1/approve', '/api/changes/prop-1/mark-applied']);
  assert.equal(editorAtMarkTime, 'refactored <script>unsafe()</script>');
  assert.equal(requests.find((request) => request.path.endsWith('/mark-applied')).body, null);
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes', 'marking applied does not save');
  assert.match(elements.get('#proposal-review-status').textContent, /Accepted into the editor · not saved/);
  assert.equal(backend.writes.length, 0);
});

test('mark-applied is not sent when Accept is declined, fails, or the proposal is rejected', async () => {
  const runs = [
    { name: 'declined', confirm: () => false, dirty: true },
    { name: 'approval failed', approve: () => jsonResponse({ error: { code: 'INTERNAL_ERROR', message: 'nope' } }, 500) },
    { name: 'rejected', reject: true },
  ];
  for (const run of runs) {
    const backend = documentBackend(run.approve ? { '/api/changes/prop-1/approve': run.approve } : {});
    const app = await createApp(backend.handler);
    const { elements, requests } = app;
    elements.get('#source-path-input').value = '/work/a.cpp';
    elements.get('#open-source-path-button').dispatch('click');
    await settle();
    if (run.dirty) typeInEditor(elements, 'int unsaved;\n');
    app.modeButtons.find((button) => button.dataset.mode === 'refactor').dispatch('click');
    elements.get('#prompt-input').value = 'Improve';
    elements.get('#language-input').value = 'C++';
    elements.get('#prompt-form').dispatch('submit');
    await settle();
    elements.get('#review-changes-button').dispatch('click');
    await settle();
    const editorBefore = elements.get('#code-editor').value;
    if (run.confirm) globalThis.window.confirm = run.confirm;
    elements.get(run.reject ? '#reject-proposal-button' : '#accept-proposal-button').dispatch('click');
    await settle();
    assert.equal(requests.filter((request) => request.path.endsWith('/mark-applied')).length, 0, run.name);
    assert.equal(elements.get('#code-editor').value, editorBefore, run.name);
  }
});

test('a failed mark-applied keeps the accepted code in the editor and says so', async () => {
  const backend = documentBackend({
    '/api/changes/prop-1/mark-applied': () => jsonResponse({
      error: { code: 'INVALID_PROPOSAL_STATE', message: 'Only approved proposals can be marked as applied.' },
    }, 409),
  });
  const app = await createApp(backend.handler);
  await openReviewedProposal(app);
  app.elements.get('#accept-proposal-button').dispatch('click');
  await settle();
  assert.equal(app.elements.get('#code-editor').value, 'refactored <script>unsafe()</script>');
  assert.equal(app.elements.get('#code-status').textContent, 'Unsaved changes');
  assert.match(app.elements.get('#notification-area').textContent,
    /accepted into the editor and not saved\. The backend could not record this \(Only approved proposals/);
});

test('Reject leaves the editor and the file on disk unchanged', async () => {
  const backend = documentBackend();
  const app = await createApp(backend.handler);
  const { elements, requests } = app;
  await openReviewedProposal(app);
  elements.get('#reject-proposal-button').dispatch('click');
  await settle();
  assert.equal(elements.get('#proposal-review-status').textContent, 'Rejected');
  assert.equal(elements.get('#code-editor').value, '// /work/a.cpp\n');
  assert.equal(elements.get('#code-status').textContent, 'Saved to disk');
  assert.equal(elements.get('#accept-proposal-button').disabled, true, 'a rejected proposal cannot be accepted');
  assert.equal(backend.writes.length, 0);
  assert.equal(requests.filter((request) => request.path === '/api/save').length, 0);
});

test('generated code can be accepted into the editor or rejected without touching it', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') return jsonResponse({ generatedCode: 'int generated();\n', language: body.language });
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  const { elements } = app;
  const sample = elements.get('#code-editor').value;
  const generate = async () => {
    elements.get('#prompt-input').value = 'Write it';
    elements.get('#language-input').value = 'C++';
    elements.get('#prompt-form').dispatch('submit');
    await settle();
  };

  await generate();
  assert.equal(elements.get('#accept-proposal-button').disabled, false);
  elements.get('#reject-proposal-button').dispatch('click');
  assert.equal(elements.get('#code-editor').value, sample);
  assert.equal(elements.get('#save-file-button').disabled, true, 'rejected output cannot be saved');
  assert.equal(elements.get('#accept-proposal-button').disabled, true);

  await generate();
  elements.get('#accept-proposal-button').dispatch('click');
  assert.equal(elements.get('#code-editor').value, 'int generated();\n');
  assert.equal(elements.get('#preview-heading').textContent, 'untitled.cpp ●');
  assert.equal(elements.get('#code-status').textContent, 'Unsaved changes');
  assert.equal(elements.get('#accept-proposal-button').disabled, true);
  assert.equal(elements.get('#save-file-button').disabled, false, 'Save output still writes the generated code as-is');
});

test('Copy puts the current editor text on the clipboard and reports failures', async () => {
  const copied = [];
  Object.defineProperty(globalThis, 'navigator', {
    value: { clipboard: { writeText: async (text) => { copied.push(text); } } },
    configurable: true,
    writable: true,
  });
  try {
    const { elements } = await createApp();
    typeInEditor(elements, 'line one\nline two\n');
    elements.get('#copy-code-button').dispatch('click');
    await settle();
    assert.deepEqual(copied, ['line one\nline two\n']);
    assert.match(elements.get('#document-status').textContent, /Copied 3 lines of untitled\.cpp/);
    assert.equal(elements.get('#document-status').dataset.tone, 'success');

    globalThis.navigator.clipboard.writeText = async () => { throw new Error('NotAllowedError'); };
    elements.get('#copy-code-button').dispatch('click');
    await settle();
    assert.match(elements.get('#document-status').textContent, /Copy failed.*Ctrl\+C/);
    assert.equal(elements.get('#document-status').dataset.tone, 'error');

    let execCommandUsed = false;
    globalThis.document.execCommand = (command) => { execCommandUsed = command === 'copy'; return true; };
    elements.get('#copy-code-button').dispatch('click');
    await settle();
    assert.equal(execCommandUsed, true, 'falls back to copying the selection');
    assert.equal(elements.get('#document-status').dataset.tone, 'success');
  } finally {
    delete globalThis.navigator;
  }
});

test('the chat records the prompt, Gemini thoughts, output, and later decisions', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') {
      return jsonResponse({
        generatedCode: 'int main() {}\n', language: body.language,
        thoughts: '**Planning**\nA <b>minimal</b> main is enough.',
      });
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  const { elements } = app;
  const conversation = elements.get('#conversation');
  elements.get('#prompt-input').value = 'Write main';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  assert.equal(elements.get('#conversation-empty').hidden, true);
  assert.equal(conversation.children.length, 2);
  assert.match(textOf(conversation.children[1]), /Thinking/);
  await new Promise((resolve) => setTimeout(resolve, 0));

  const [user, reply] = conversation.children;
  assert.match(textOf(user), /YOU · Generate · C\+\+/);
  assert.match(textOf(user), /Write main/);
  assert.equal(reply.dataset.state, 'done');
  const thoughts = reply.children.find((child) => child.className === 'message-thoughts');
  assert.ok(thoughts, 'thoughts are shown');
  assert.match(textOf(thoughts), /Planning/);
  assert.match(textOf(thoughts), /A <b>minimal<\/b> main is enough\./, 'thoughts stay plain text');
  assert.match(textOf(reply), /Generated 1 line of C\+\+/);
  assert.equal(reply.children.find((child) => child.className === 'message-code').textContent, 'int main() {}\n');
  assert.equal(elements.get('#prompt-input').value, '', 'the prompt is cleared after a reply');

  elements.get('#accept-proposal-button').dispatch('click');
  assert.match(textOf(conversation.children.at(-1)), /accepted into the editor/);

  elements.get('#clear-chat-button').dispatch('click');
  assert.deepEqual(conversation.children, [elements.get('#conversation-empty')]);
  assert.equal(elements.get('#conversation-empty').hidden, false);
});

test('a failed request is shown as an error reply in the chat', async () => {
  const app = await createApp((path, body, fallback) => {
    if (path === '/api/generate') {
      return jsonResponse({ error: { code: 'GENERATION_FAILED', message: 'Code generation failed.' } }, 502);
    }
    return fallback(`http://127.0.0.1:8080${path}`, { method: body ? 'POST' : 'GET', body: body && JSON.stringify(body) });
  });
  const { elements } = app;
  elements.get('#prompt-input').value = 'Write main';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-form').dispatch('submit');
  await new Promise((resolve) => setTimeout(resolve, 0));
  const reply = elements.get('#conversation').children.at(-1);
  assert.equal(reply.dataset.state, 'error');
  assert.match(textOf(reply), /GENERATION_FAILED/);
  assert.equal(elements.get('#prompt-input').value, 'Write main', 'the prompt is kept for a retry');
});

test('Enter in the prompt sends it and Shift+Enter does not', async () => {
  const { elements, requests } = await createApp();
  elements.get('#prompt-input').value = 'Write main';
  elements.get('#language-input').value = 'C++';
  elements.get('#prompt-input').dispatch('keydown', { key: 'Enter', shiftKey: true });
  assert.equal(requests.filter((request) => request.path === '/api/generate').length, 0);
  elements.get('#prompt-input').dispatch('keydown', { key: 'Enter', shiftKey: false });
  assert.equal(requests.filter((request) => request.path === '/api/generate').length, 1);
});
