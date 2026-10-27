import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const apiSource = readFileSync(new URL('../js/api.js', import.meta.url), 'utf8');
const apiModuleUrl = `data:text/javascript;base64,${Buffer.from(apiSource).toString('base64')}`;
const { ApiError, createApiClient, getBackendBaseUrl } = await import(apiModuleUrl);

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
