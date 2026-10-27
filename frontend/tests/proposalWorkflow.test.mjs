import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const workflowSource = readFileSync(new URL('../js/proposalWorkflow.js', import.meta.url), 'utf8');
const workflowUrl = `data:text/javascript;base64,${Buffer.from(workflowSource).toString('base64')}`;
const { createProposalWorkflow } = await import(workflowUrl);

function newWorkflow(overrides = {}) {
  const api = {
    getProposalDiff: async () => ({ diff: '--- old\n+++ new\n' }),
    approveProposal: async (proposalId) => ({ proposalId, status: 'APPROVED' }),
    rejectProposal: async (proposalId) => ({ proposalId, status: 'REJECTED' }),
    saveProposal: async () => ({ savedPath: '/tmp/main.cpp', status: 'SAVED' }),
    ...overrides,
  };
  const workflow = createProposalWorkflow(api);
  workflow.setCurrentSourceCode('old');
  workflow.setProposal({
    proposalId: 'prop-1',
    status: 'PENDING',
    originalCode: 'old',
    proposedCode: 'new',
  });
  return { api, workflow };
}

test('review results and decisions update lifecycle only after API confirmation', async () => {
  let releaseDiff;
  let approvalCalls = 0;
  const { workflow } = newWorkflow({
    getProposalDiff: () => new Promise((resolve) => { releaseDiff = resolve; }),
    approveProposal: async (proposalId) => {
      approvalCalls += 1;
      return { proposalId, status: 'APPROVED' };
    },
  });

  const review = workflow.review();
  assert.equal(workflow.getState().proposal.reviewed, false);
  assert.equal(await workflow.review(), false);
  releaseDiff({ diff: 'reviewed diff' });
  assert.equal(await review, true);
  assert.equal(workflow.getState().proposal.reviewed, true);
  assert.equal(workflow.getState().diff, 'reviewed diff');
  assert.equal(await workflow.decide('approve'), true);
  assert.equal(approvalCalls, 1);
  assert.equal(workflow.getState().proposal.status, 'APPROVED');
});

test('a failed decision never marks a proposal approved or permits another action', async () => {
  const { workflow } = newWorkflow({
    getProposalDiff: async () => ({ diff: 'diff' }),
    approveProposal: async () => { throw new Error('decision unavailable'); },
  });
  await workflow.review();
  assert.equal(await workflow.decide('approve'), false);
  assert.equal(workflow.getState().proposal.status, 'PENDING');
  assert.equal(workflow.getState().decisionBlocked, true);
  assert.equal(await workflow.decide('reject'), false);
});

test('a confirmed rejection updates state and never enables saving', async () => {
  const { workflow } = newWorkflow({
    getProposalDiff: async () => ({ diff: 'diff' }),
    rejectProposal: async (proposalId) => ({ proposalId, status: 'REJECTED' }),
  });
  await workflow.review();
  assert.equal(await workflow.decide('reject'), true);
  assert.equal(workflow.getState().proposal.status, 'REJECTED');
  assert.equal(await workflow.save('main.cpp'), false);
});

test('editor changes block approval and saving until the proposal source is restored', async () => {
  const { workflow } = newWorkflow();
  await workflow.review();
  workflow.setCurrentSourceCode('edited after proposal');
  assert.equal(await workflow.decide('approve'), false);
  assert.equal(workflow.getState().proposal.status, 'PENDING');
  assert.equal(workflow.getState().proposal.sourceMatches, false);
  workflow.setCurrentSourceCode('old');
  assert.equal(await workflow.decide('approve'), true);
  workflow.setCurrentSourceCode('changed after approval');
  assert.equal(await workflow.save('main.cpp'), false);
  assert.equal(workflow.getState().proposal.status, 'APPROVED');
});

test('saving requires confirmed approval and applies only after confirmed save', async () => {
  let saves = 0;
  const { workflow } = newWorkflow({
    getProposalDiff: async () => ({ diff: 'diff' }),
    approveProposal: async (proposalId) => ({ proposalId, status: 'APPROVED' }),
    saveProposal: async () => {
      saves += 1;
      if (saves === 1) throw new Error('disk unavailable');
      return { savedPath: '/tmp/main.cpp', status: 'SAVED' };
    },
  });
  assert.equal(await workflow.save('main.cpp'), false);
  assert.equal(saves, 0);
  await workflow.review();
  await workflow.decide('approve');
  assert.equal(await workflow.save('main.cpp'), false);
  assert.equal(workflow.getState().proposal.status, 'APPROVED');
  assert.equal(workflow.getState().savedPath, '');
  assert.equal(await workflow.save('main.cpp'), true);
  assert.equal(workflow.getState().proposal.status, 'APPLIED');
  assert.equal(workflow.getState().savedPath, '/tmp/main.cpp');
});

test('overwrite is retried only after explicit confirmation', async () => {
  const requests = [];
  const { workflow } = newWorkflow({
    getProposalDiff: async () => ({ diff: 'diff' }),
    approveProposal: async (proposalId) => ({ proposalId, status: 'APPROVED' }),
    saveProposal: async (request) => {
      requests.push(request);
      if (!request.overwriteConfirmed) {
        const error = new Error('confirmation required');
        error.code = 'OVERWRITE_CONFIRMATION_REQUIRED';
        throw error;
      }
      return { savedPath: '/tmp/main.cpp', status: 'SAVED' };
    },
  });
  await workflow.review();
  await workflow.decide('approve');
  let confirmations = 0;
  assert.equal(await workflow.save('main.cpp', async () => { confirmations += 1; return false; }), false);
  assert.equal(confirmations, 1);
  assert.equal(requests.length, 1);
  assert.equal(workflow.getState().proposal.status, 'APPROVED');
  assert.equal(await workflow.save('main.cpp', async () => true), true);
  assert.equal(requests.length, 3);
  assert.equal(requests[1].overwriteConfirmed, false);
  assert.equal(requests[2].overwriteConfirmed, true);
});

test('repeated save clicks during one request send a single backend request', async () => {
  let releaseSave;
  let saveCalls = 0;
  const { workflow } = newWorkflow({
    getProposalDiff: async () => ({ diff: 'diff' }),
    approveProposal: async (proposalId) => ({ proposalId, status: 'APPROVED' }),
    saveProposal: () => {
      saveCalls += 1;
      return new Promise((resolve) => { releaseSave = resolve; });
    },
  });
  await workflow.review();
  await workflow.decide('approve');
  const saving = workflow.save('main.cpp');
  assert.equal(await workflow.save('main.cpp'), false);
  assert.equal(saveCalls, 1);
  releaseSave({ savedPath: '/tmp/main.cpp', status: 'SAVED' });
  assert.equal(await saving, true);
  assert.equal(workflow.getState().proposal.status, 'APPLIED');
});

test('generated source and API errors are rendered as text, never HTML', () => {
  const appSource = readFileSync(new URL('../js/app.js', import.meta.url), 'utf8');
  assert.equal(appSource.includes('.innerHTML'), false);
  assert.match(appSource, /\.textContent\s*=/);
});
