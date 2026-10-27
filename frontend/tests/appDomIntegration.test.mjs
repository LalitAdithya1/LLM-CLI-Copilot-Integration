import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const appSource = readFileSync(new URL('../js/app.js', import.meta.url), 'utf8');
const htmlSource = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const apiSource = readFileSync(new URL('../js/api.js', import.meta.url), 'utf8');
const workflowSource = readFileSync(new URL('../js/proposalWorkflow.js', import.meta.url), 'utf8');

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
  replaceChildren(fragment) { this.children = fragment.children; }
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
  };
  const requests = [];
  const defaultFetch = async (url, options) => {
    const path = new URL(url).pathname;
    const body = options.body ? JSON.parse(options.body) : null;
    requests.push({ path, method: options.method, body });
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
      return fetchOverride(path, body, defaultFetch);
    }
    : defaultFetch;
  const apiUrl = dataUrl(apiSource, Math.random());
  const workflowUrl = dataUrl(workflowSource, Math.random());
  const rewrittenApp = appSource
    .replace("from './api.js'", `from '${apiUrl}'`)
    .replace("from './proposalWorkflow.js'", `from '${workflowUrl}'`);
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
  assert.equal(saveButton.disabled, false);
  assert.deepEqual(requests.find((request) => request.path.endsWith('/approve')).body, null);

  elements.get('#save-file-name').value = 'main.cpp';
  saveButton.dispatch('click');
  assert.equal(saveButton.disabled, true);
  assert.match(saveButton.textContent, /Saving/);
  saveButton.dispatch('click');
  assert.equal(requests.filter((request) => request.path === '/api/save').length, 1);
  assert.deepEqual(requests.find((request) => request.path === '/api/save').body, {
    proposalId: 'prop-1', fileName: 'main.cpp', overwriteConfirmed: false,
  });
  save.resolve(jsonResponse({ savedPath: '/tmp/main.cpp', status: 'SAVED' }));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.match(elements.get('#proposal-review-status').textContent, /Saved and applied/);
  assert.match(elements.get('#notification-area').textContent, /confirmed the save/);
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

test('failed save keeps the proposal approved and reports failure instead of applied success', async () => {
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
  app.elements.get('#save-file-name').value = 'main.cpp';
  app.elements.get('#save-file-button').dispatch('click');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get('#proposal-review-status').textContent, 'Approved · not saved');
  assert.match(app.elements.get('#proposal-review-availability').textContent, /File could not be saved/);
  assert.doesNotMatch(app.elements.get('#notification-area').textContent, /confirmed the save/);
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
