export function createProposalWorkflow(api, onChange = () => {}) {
  let currentEditorSource = '';
  let state = {
    proposal: null,
    diff: null,
    operation: null,
    error: '',
    savedPath: '',
    decisionBlocked: false,
  };

  function publish() {
    onChange({
      ...state,
      proposal: state.proposal ? { ...state.proposal } : null,
    });
  }

  function setProposal(proposal) {
    state = {
      proposal: {
        id: proposal.proposalId,
        status: proposal.status,
        originalCode: proposal.originalCode,
        proposedCode: proposal.proposedCode,
        reviewed: false,
        sourceMatches: currentEditorSource === proposal.originalCode,
      },
      diff: null,
      operation: null,
      error: '',
      savedPath: '',
      decisionBlocked: false,
    };
    publish();
  }

  function setCurrentSourceCode(sourceCode) {
    currentEditorSource = sourceCode;
    if (state.proposal) {
      state = {
        ...state,
        proposal: { ...state.proposal, sourceMatches: sourceCode === state.proposal.originalCode },
      };
      publish();
    }
  }

  function reset() {
    state = {
      proposal: null,
      diff: null,
      operation: null,
      error: '',
      savedPath: '',
      decisionBlocked: false,
    };
    publish();
  }

  async function review() {
    if (state.operation || state.proposal?.status !== 'PENDING') return false;
    const proposalId = state.proposal.id;
    state = { ...state, operation: 'review', error: '' };
    publish();
    try {
      const result = await api.getProposalDiff(proposalId);
      if (state.proposal?.id !== proposalId) return false;
      state = {
        ...state,
        diff: result.diff,
        proposal: { ...state.proposal, reviewed: true },
      };
      return true;
    } catch (error) {
      state = { ...state, error: error.message || 'The proposal diff could not be loaded.' };
      return false;
    } finally {
      state = { ...state, operation: null };
      publish();
    }
  }

  async function decide(action) {
    if (state.operation || state.proposal?.status !== 'PENDING'
      || !state.proposal.reviewed || !state.proposal.sourceMatches || state.decisionBlocked
      || (action !== 'approve' && action !== 'reject')) return false;
    const proposalId = state.proposal.id;
    state = { ...state, operation: action, error: '' };
    publish();
    try {
      const result = action === 'approve'
        ? await api.approveProposal(proposalId)
        : await api.rejectProposal(proposalId);
      const expectedStatus = action === 'approve' ? 'APPROVED' : 'REJECTED';
      if (result.status !== expectedStatus || result.proposalId !== proposalId) {
        throw new Error('The backend did not confirm the requested proposal decision.');
      }
      state = { ...state, proposal: { ...state.proposal, status: expectedStatus } };
      return true;
    } catch (error) {
      state = {
        ...state,
        error: error.message || 'The proposal decision could not be confirmed.',
        decisionBlocked: true,
      };
      return false;
    } finally {
      state = { ...state, operation: null };
      publish();
    }
  }

  async function save(fileName, confirmOverwrite) {
    if (state.operation || state.proposal?.status !== 'APPROVED'
      || !state.proposal.sourceMatches
      || typeof fileName !== 'string' || fileName.trim().length === 0) return false;
    const proposalId = state.proposal.id;
    state = { ...state, operation: 'save', error: '' };
    publish();
    try {
      let result;
      try {
        result = await api.saveProposal({ proposalId, fileName, overwriteConfirmed: false });
      } catch (error) {
        if (error.code !== 'OVERWRITE_CONFIRMATION_REQUIRED') throw error;
        if (typeof confirmOverwrite !== 'function' || !await confirmOverwrite(fileName)) {
          state = { ...state, error: 'Overwrite was not confirmed. The approved proposal remains available to save.' };
          return false;
        }
        result = await api.saveProposal({ proposalId, fileName, overwriteConfirmed: true });
      }
      if (result.status !== 'SAVED' || typeof result.savedPath !== 'string'
        || result.savedPath.trim().length === 0) {
        throw new Error('The backend did not confirm that the proposal was saved.');
      }
      state = {
        ...state,
        proposal: { ...state.proposal, status: 'APPLIED' },
        savedPath: result.savedPath,
      };
      return true;
    } catch (error) {
      state = { ...state, error: error.message || 'The proposal save could not be confirmed.' };
      return false;
    } finally {
      state = { ...state, operation: null };
      publish();
    }
  }

  return Object.freeze({
    getState: () => ({ ...state, proposal: state.proposal ? { ...state.proposal } : null }),
    setProposal,
    setCurrentSourceCode,
    reset,
    review,
    decide,
    save,
  });
}
