import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const apiSource = readFileSync(new URL('../js/api.js', import.meta.url), 'utf8');
const apiModuleUrl = `data:text/javascript;base64,${Buffer.from(apiSource).toString('base64')}`;
const {
  ApiError, SESSION_TOKEN_HEADER, createApiClient, getBackendBaseUrl, takeBootstrapCode,
} = await import(apiModuleUrl);

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

test('proposal diff uses the backend proposal schema and validates its response', async () => {
  let captured;
  const client = createApiClient({
    baseUrl: 'http://127.0.0.1:8080/',
    fetchImpl: async (url, options) => {
      captured = { url, options };
      return jsonResponse({ diff: '--- original\n+++ proposed\n' });
    },
  });

  assert.deepEqual(await client.getProposalDiff('prop-7'), {
    diff: '--- original\n+++ proposed\n',
  });
  assert.equal(captured.url, 'http://127.0.0.1:8080/api/diff');
  assert.equal(captured.options.method, 'POST');
  assert.deepEqual(JSON.parse(captured.options.body), { proposalId: 'prop-7' });
});

test('proposal save sends only supported fields and validates saved path', async () => {
  let captured;
  const client = createApiClient({
    fetchImpl: async (url, options) => {
      captured = { url, options };
      return jsonResponse({ savedPath: '/tmp/example.cpp', status: 'SAVED' });
    },
  });

  assert.deepEqual(await client.saveProposal({
    proposalId: 'prop-8',
    fileName: 'example.cpp',
    overwriteConfirmed: true,
  }), { savedPath: '/tmp/example.cpp', status: 'SAVED' });
  assert.equal(captured.url, 'http://127.0.0.1:8080/api/save');
  assert.equal(captured.options.method, 'POST');
  assert.deepEqual(JSON.parse(captured.options.body), {
    proposalId: 'prop-8',
    fileName: 'example.cpp',
    overwriteConfirmed: true,
  });
});

test('diff and save report safe backend errors without losing the status code', async () => {
  const client = createApiClient({
    fetchImpl: async () => jsonResponse({
      error: { code: 'PROPOSAL_NOT_FOUND', message: 'Proposal was not found.' },
    }, 404),
  });

  await assert.rejects(client.getProposalDiff('prop-missing'), (error) => {
    assert.ok(error instanceof ApiError);
    assert.equal(error.code, 'PROPOSAL_NOT_FOUND');
    assert.equal(error.status, 404);
    return true;
  });
  await assert.rejects(client.saveProposal({ proposalId: 'prop-1', fileName: 'main.cpp' }), (error) => {
    assert.ok(error instanceof ApiError);
    assert.equal(error.code, 'PROPOSAL_NOT_FOUND');
    assert.equal(error.status, 404);
    return true;
  });
});

test('malformed JSON and invalid success payloads fail clearly', async () => {
  const malformedClient = createApiClient({
    fetchImpl: async () => new Response('{', { status: 200 }),
  });
  await assert.rejects(malformedClient.getProposalDiff('prop-1'), (error) => {
    assert.equal(error.code, 'INVALID_RESPONSE');
    return true;
  });

  const invalidSaveClient = createApiClient({
    fetchImpl: async () => jsonResponse({ savedPath: 'relative.cpp', status: 'SAVED' }),
  });
  await assert.rejects(
    invalidSaveClient.saveProposal({ proposalId: 'prop-1', fileName: 'main.cpp' }),
    (error) => error.code === 'INVALID_RESPONSE',
  );
});

test('network failures and invalid request inputs are reported', async () => {
  const client = createApiClient({ fetchImpl: async () => { throw new Error('private detail'); } });
  await assert.rejects(client.getProposalDiff('prop-1'), (error) => {
    assert.equal(error.code, 'NETWORK_ERROR');
    assert.equal(error.message.includes('private detail'), false);
    return true;
  });
  await assert.rejects(client.getProposalDiff(' '), (error) => error.code === 'INVALID_REQUEST');
  await assert.rejects(
    client.saveProposal({ proposalId: 'prop-1', fileName: 'main.cpp', overwriteConfirmed: 'yes' }),
    (error) => error.code === 'INVALID_REQUEST',
  );
});

test('backend URL is same-origin when the C++ server serves the frontend', () => {
  assert.equal(
    getBackendBaseUrl(new URL('http://127.0.0.1:8080/index.html')),
    'http://127.0.0.1:8080',
  );
  assert.equal(getBackendBaseUrl(new URL('http://localhost:9000/')), 'http://localhost:9000');
  assert.equal(getBackendBaseUrl(new URL('http://127.0.0.1:5500/')), 'http://127.0.0.1:8080');
  assert.equal(getBackendBaseUrl(new URL('http://evil.example:8080/')), 'http://127.0.0.1:8080');
  assert.equal(getBackendBaseUrl(undefined), 'http://127.0.0.1:8080');
});

test('source file open and save-back send exact fields and validate responses', async () => {
  const captured = [];
  const responses = [
    { path: '/work/a.cpp', content: 'int a;\n', revision: 'fnv1a64:0123456789abcdef' },
    { savedPath: '/work/a.cpp', status: 'SAVED', revision: 'fnv1a64:fedcba9876543210' },
    { path: 'relative.cpp', content: '', revision: 'fnv1a64:0123456789abcdef' },
    { savedPath: '/work/other.cpp', status: 'SAVED', revision: 'fnv1a64:fedcba9876543210' },
  ];
  const client = createApiClient({
    fetchImpl: async (url, options) => {
      captured.push({ url, body: JSON.parse(options.body) });
      return jsonResponse(responses.shift());
    },
  });
  assert.equal((await client.readSourceFile('/work/a.cpp')).content, 'int a;\n');
  assert.equal((await client.writeSourceFile({
    path: '/work/a.cpp', content: 'int b;\n', baseRevision: 'fnv1a64:0123456789abcdef', overwriteConfirmed: false,
  })).revision, 'fnv1a64:fedcba9876543210');
  assert.deepEqual(captured.map((request) => request.url), [
    'http://127.0.0.1:8080/api/files/read', 'http://127.0.0.1:8080/api/files/write',
  ]);
  assert.deepEqual(captured[0].body, { path: '/work/a.cpp' });
  assert.deepEqual(captured[1].body, {
    path: '/work/a.cpp', content: 'int b;\n', baseRevision: 'fnv1a64:0123456789abcdef', overwriteConfirmed: false,
  });
  await assert.rejects(client.readSourceFile('/work/a.cpp'), (error) => error.code === 'INVALID_RESPONSE');
  await assert.rejects(client.writeSourceFile({
    path: '/work/a.cpp', content: 'x', baseRevision: 'fnv1a64:0123456789abcdef', overwriteConfirmed: false,
  }), (error) => error.code === 'INVALID_RESPONSE', 'a save confirming another path is rejected');
  await assert.rejects(client.readSourceFile('  '), (error) => error.code === 'INVALID_REQUEST');
  await assert.rejects(
    client.writeSourceFile({ path: '/work/a.cpp', content: 'x', baseRevision: 'r' }),
    (error) => error.code === 'INVALID_REQUEST',
  );
});

test('markProposalApplied posts an empty body and requires an APPLIED confirmation', async () => {
  const captured = [];
  const responses = [
    { proposalId: 'prop-3', status: 'APPLIED' },
    { proposalId: 'prop-3', status: 'APPROVED' },
  ];
  const client = createApiClient({
    fetchImpl: async (url, options) => {
      captured.push({ url, options });
      return jsonResponse(responses.shift());
    },
  });
  assert.deepEqual(await client.markProposalApplied('prop-3'), { proposalId: 'prop-3', status: 'APPLIED' });
  assert.equal(captured[0].url, 'http://127.0.0.1:8080/api/changes/prop-3/mark-applied');
  assert.equal(captured[0].options.method, 'POST');
  assert.equal(captured[0].options.body, undefined);
  await assert.rejects(client.markProposalApplied('prop-3'), (error) => error.code === 'INVALID_RESPONSE');
  await assert.rejects(client.markProposalApplied(' '), (error) => error.code === 'INVALID_REQUEST');
});

test('generated-code save sends exactly the backend save fields', async () => {
  let captured;
  const client = createApiClient({
    fetchImpl: async (url, options) => {
      captured = { url, options };
      return jsonResponse({ savedPath: '/tmp/gen.cpp', status: 'SAVED' });
    },
  });
  assert.deepEqual(
    await client.saveGeneratedCode({ fileName: 'gen.cpp', content: 'int x;\n', overwriteConfirmed: false }),
    { savedPath: '/tmp/gen.cpp', status: 'SAVED' },
  );
  assert.equal(captured.url, 'http://127.0.0.1:8080/api/save');
  assert.deepEqual(JSON.parse(captured.options.body), {
    fileName: 'gen.cpp', content: 'int x;\n', overwriteConfirmed: false,
  });
  await assert.rejects(
    client.saveGeneratedCode({ fileName: 'gen.cpp', content: '  ', overwriteConfirmed: false }),
    (error) => error.code === 'INVALID_REQUEST',
  );
  await assert.rejects(
    client.saveGeneratedCode({ fileName: 'gen.cpp', content: 'int x;' }),
    (error) => error.code === 'INVALID_REQUEST',
  );
});

test('generation and refactoring responses accept an optional thoughts string', async () => {
  const respond = (payload) => createApiClient({ fetchImpl: async () => jsonResponse(payload) });
  const generation = { generatedCode: 'int main() {}', language: 'C++' };
  assert.deepEqual(await respond(generation).generateCode({ instruction: 'x', language: 'C++' }), generation);
  assert.equal(
    (await respond({ ...generation, thoughts: 'Plan' }).generateCode({ instruction: 'x', language: 'C++' })).thoughts,
    'Plan',
  );
  await assert.rejects(
    respond({ ...generation, thoughts: 7 }).generateCode({ instruction: 'x', language: 'C++' }),
    (error) => error.code === 'INVALID_RESPONSE',
  );
  await assert.rejects(
    respond({ ...generation, extra: '' }).generateCode({ instruction: 'x', language: 'C++' }),
    (error) => error.code === 'INVALID_RESPONSE',
  );

  const proposal = {
    proposalId: 'prop-1', status: 'PENDING', originalCode: 'a', proposedCode: 'b',
    baseEditorHash: 'fnv1a64:0123456789abcdef', thoughts: 'Rename it.',
  };
  const request = { sourceCode: 'a', instruction: 'x', language: 'C++', editorRevision: 'r1' };
  assert.equal((await respond(proposal).refactorCode(request)).thoughts, 'Rename it.');
});

const TOKEN = '0123456789abcdef'.repeat(4);

test('the session token is obtained once and sent on every later request', async () => {
  const calls = [];
  const client = createApiClient({
    baseUrl: 'http://127.0.0.1:8080',
    fetchImpl: async (url, options) => {
      calls.push({ path: new URL(url).pathname, options });
      if (url.endsWith('/api/session')) return jsonResponse({ token: TOKEN });
      return jsonResponse({ status: 'ok' });
    },
  });
  assert.equal(client.hasSession(), false);
  await client.getHealth();
  assert.equal(calls[0].options.headers[SESSION_TOKEN_HEADER], undefined, 'no token before a session');

  await client.startSession('c'.repeat(64));
  assert.equal(client.hasSession(), true);
  const session = calls[1];
  assert.equal(session.path, '/api/session');
  assert.deepEqual(JSON.parse(session.options.body), { bootstrap: 'c'.repeat(64) });
  assert.equal(session.options.credentials, 'include');
  assert.equal(session.options.headers[SESSION_TOKEN_HEADER], undefined);

  await client.getHealth();
  assert.equal(calls[2].options.headers[SESSION_TOKEN_HEADER], TOKEN);

  // Without a code the session cookie is used, e.g. after a reload.
  await client.startSession(null);
  assert.deepEqual(JSON.parse(calls[3].options.body), {});
});

test('an invalid session response or a rejected token leaves no usable session', async () => {
  for (const payload of [{ token: 'short' }, { token: TOKEN, extra: 1 }, { token: 7 }]) {
    const client = createApiClient({ fetchImpl: async () => jsonResponse(payload) });
    await assert.rejects(client.startSession('x'), (error) => error.code === 'INVALID_RESPONSE');
    assert.equal(client.hasSession(), false);
  }

  const failing = createApiClient({
    fetchImpl: async () => jsonResponse({ error: { code: 'INVALID_BOOTSTRAP', message: 'This startup link is invalid.' } }, 401),
  });
  await assert.rejects(failing.startSession('x'), (error) => error.code === 'INVALID_BOOTSTRAP' && error.status === 401);

  let unauthorized = false;
  const client = createApiClient({
    fetchImpl: async (url) => {
      if (url.endsWith('/api/session')) return jsonResponse({ token: TOKEN });
      return unauthorized
        ? jsonResponse({ error: { code: 'UNAUTHORIZED', message: 'Missing or invalid session token.' } }, 401)
        : jsonResponse({ status: 'ok' });
    },
  });
  await client.startSession('x');
  unauthorized = true;
  await assert.rejects(client.getHealth(), (error) => error.code === 'UNAUTHORIZED');
  assert.equal(client.hasSession(), false, 'a token the server rejects is dropped');
});

test('takeBootstrapCode reads the one-time code and removes it from the address bar', () => {
  const replaced = [];
  const history = { replaceState: (...args) => replaced.push(args) };
  const code = 'd'.repeat(64);
  assert.equal(takeBootstrapCode({ hash: `#bootstrap=${code}`, pathname: '/', search: '?x=1' }, history), code);
  assert.deepEqual(replaced, [[null, '', '/?x=1']]);

  assert.equal(takeBootstrapCode({ hash: '#bootstrap=not-a-code', pathname: '/', search: '' }, history), null);
  assert.equal(replaced.length, 2, 'a malformed code is removed too');

  assert.equal(takeBootstrapCode({ hash: '', pathname: '/', search: '' }, history), null);
  assert.equal(takeBootstrapCode({ hash: '#other', pathname: '/', search: '' }, history), null);
  assert.equal(replaced.length, 2);
  assert.equal(takeBootstrapCode(undefined, undefined), null);
});

test('configuration no longer carries the supported-extension list', async () => {
  const config = { launchDirectory: '/a', activeOutputDirectory: '/b' };
  const client = createApiClient({ fetchImpl: async () => jsonResponse(config) });
  assert.deepEqual(await client.getConfig(), config);
  const extended = createApiClient({
    fetchImpl: async () => jsonResponse({ ...config, supportedExtensions: ['.cpp'] }),
  });
  await assert.rejects(extended.getConfig(), (error) => error.code === 'INVALID_RESPONSE');
});
