import {
  ApiError,
  approveProposal,
  generateCode,
  getConfig,
  getHealth,
  refactorCode,
  rejectProposal,
} from './api.js';

const SAMPLE_ORIGINAL_CODE = `#include <iostream>

int main() {
    std::cout << "Hello, CodeAgent!\\n";
    return 0;
}
`;

const REQUIRED_ELEMENTS = [
  '[data-app="codeagent"]',
  '#main-content',
  '#assistant-panel',
  '#preview-panel',
  '#notification-area',
  '#mode-generate',
  '#mode-refactor',
  '#prompt-form',
  '#prompt-input',
  '#language-input',
  '#prompt-feedback',
  '#code-editor',
  '#editor-line-numbers',
  '#code-status',
  '#assistant-status',
  '#assistant-status-message',
  '#assistant-status-detail',
  '#backend-retry-button',
  '#backend-config-status',
  '#backend-config-values',
  '#backend-config-extensions',
  '#backend-config-launch-directory',
  '#backend-config-output-directory',
  '#backend-config-error',
  '#generate-submit-button',
  '#generate-code-button',
  '#proposal-review-status',
  '#proposal-review-availability',
  '#proposal-original-code',
  '#proposal-proposed-code',
  '#accept-proposal-button',
  '#reject-proposal-button',
  '#save-file-button',
];

let appInitializationStarted = false;

function initializeApp() {
  if (appInitializationStarted) {
    return;
  }

  const missingElements = REQUIRED_ELEMENTS.filter(
    (selector) => document.querySelector(selector) === null,
  );

  if (missingElements.length > 0) {
    console.error(
      `CodeAgent could not initialize. Missing required elements: ${missingElements.join(', ')}`,
    );
    return;
  }
  appInitializationStarted = true;

  const modeButtons = [...document.querySelectorAll('.mode-card[data-mode]')];
  const promptForm = document.querySelector('#prompt-form');
  const promptInput = document.querySelector('#prompt-input');
  const languageInput = document.querySelector('#language-input');
  const promptFeedback = document.querySelector('#prompt-feedback');
  const notificationArea = document.querySelector('#notification-area');
  const codeEditor = document.querySelector('#code-editor');
  const editorLineNumbers = document.querySelector('#editor-line-numbers');
  const codeStatus = document.querySelector('#code-status');
  const assistantStatus = document.querySelector('#assistant-status');
  const assistantStatusMessage = document.querySelector('#assistant-status-message');
  const assistantStatusDetail = document.querySelector('#assistant-status-detail');
  const backendRetryButton = document.querySelector('#backend-retry-button');
  const backendConfigStatus = document.querySelector('#backend-config-status');
  const backendConfigValues = document.querySelector('#backend-config-values');
  const backendConfigExtensions = document.querySelector('#backend-config-extensions');
  const backendConfigLaunchDirectory = document.querySelector('#backend-config-launch-directory');
  const backendConfigOutputDirectory = document.querySelector('#backend-config-output-directory');
  const backendConfigError = document.querySelector('#backend-config-error');
  const proposalReviewStatus = document.querySelector('#proposal-review-status');
  const proposalReviewAvailability = document.querySelector('#proposal-review-availability');
  const proposalOriginalCode = document.querySelector('#proposal-original-code');
  const proposalProposedCode = document.querySelector('#proposal-proposed-code');
  const acceptProposalButton = document.querySelector('#accept-proposal-button');
  const rejectProposalButton = document.querySelector('#reject-proposal-button');
  const saveFileButton = document.querySelector('#save-file-button');
  const submitButtons = [
    document.querySelector('#generate-submit-button'),
    document.querySelector('#generate-code-button'),
  ];
  let workflowInProgress = false;
  let backendRefreshPromise = null;
  let editorRevisionSequence = 0;
  let selectedMode = 'generate';
  let activeProposal = null;
  const originalCode = SAMPLE_ORIGINAL_CODE;
  let draftCode = originalCode;

  function renderDecisionControls() {
    const canDecide = activeProposal?.status === 'PENDING' && !workflowInProgress;
    acceptProposalButton.disabled = !canDecide;
    rejectProposalButton.disabled = !canDecide;
    saveFileButton.disabled = true;
  }

  function setWorkflowInProgress(isInProgress, actionLabel = '') {
    workflowInProgress = isInProgress;
    promptInput.disabled = isInProgress;
    languageInput.disabled = isInProgress;
    modeButtons.forEach((button) => {
      button.disabled = isInProgress || button.dataset.mode === 'files';
    });
    submitButtons.forEach((button) => {
      button.disabled = isInProgress;
      button.textContent = isInProgress
        ? actionLabel
        : selectedMode === 'refactor' ? 'Create proposal' : 'Generate code';
    });
    renderDecisionControls();
  }

  function renderLineNumbers() {
    const fragment = document.createDocumentFragment();
    const lineCount = draftCode.split('\n').length;

    for (let lineNumber = 1; lineNumber <= lineCount; lineNumber += 1) {
      const line = document.createElement('li');
      line.textContent = String(lineNumber);
      fragment.append(line);
    }

    editorLineNumbers.replaceChildren(fragment);
    editorLineNumbers.scrollTop = codeEditor.scrollTop;
  }

  function renderCodeStatus() {
    codeStatus.textContent = draftCode === originalCode
      ? 'Unmodified'
      : 'Unsaved changes';
  }

  function renderEditorState() {
    renderLineNumbers();
    renderCodeStatus();
  }

  codeEditor.value = originalCode;
  renderEditorState();

  codeEditor.addEventListener('input', () => {
    draftCode = codeEditor.value;
    renderEditorState();
  });

  codeEditor.addEventListener('scroll', () => {
    editorLineNumbers.scrollTop = codeEditor.scrollTop;
  }, { passive: true });

  function renderStatus(message) {
    notificationArea.textContent = message;
  }

  function apiErrorMessage(error, fallback) {
    return error instanceof ApiError && error.message.trim().length > 0
      ? error.message
      : fallback;
  }

  async function refreshBackendHealth() {
    assistantStatus.dataset.state = 'checking';
    assistantStatusMessage.textContent = 'Checking backend…';
    assistantStatusDetail.textContent = '';

    try {
      const health = await getHealth();
      if (health.status !== 'ok') {
        throw new ApiError(
          'INVALID_RESPONSE',
          'The backend returned an invalid health response.',
        );
      }
      assistantStatus.dataset.state = 'connected';
      assistantStatusMessage.textContent = 'Backend connected';
    } catch (error) {
      assistantStatus.dataset.state = 'unavailable';
      assistantStatusMessage.textContent = 'Backend unavailable';
      assistantStatusDetail.textContent = apiErrorMessage(
        error,
        'Could not connect to the local backend.',
      );
    }
  }

  async function refreshBackendConfig() {
    backendConfigStatus.textContent = 'Loading configuration…';
    backendConfigValues.hidden = true;
    backendConfigError.hidden = true;
    backendConfigError.textContent = '';

    try {
      const config = await getConfig();
      backendConfigExtensions.textContent = config.supportedExtensions.length > 0
        ? config.supportedExtensions.join(', ')
        : 'No extensions reported';
      backendConfigLaunchDirectory.textContent = config.launchDirectory.trim()
        ? config.launchDirectory
        : 'No launch directory reported';
      backendConfigOutputDirectory.textContent = config.activeOutputDirectory.trim()
        ? config.activeOutputDirectory
        : 'No output directory reported';
      backendConfigStatus.textContent = 'Configuration loaded';
      backendConfigValues.hidden = false;
    } catch (error) {
      backendConfigStatus.textContent = 'Configuration unavailable';
      backendConfigError.textContent = apiErrorMessage(
        error,
        'Could not load backend configuration.',
      );
      backendConfigError.hidden = false;
    }
  }

  function refreshBackendInformation() {
    if (backendRefreshPromise) {
      return backendRefreshPromise;
    }

    backendRetryButton.disabled = true;
    backendRefreshPromise = Promise.all([
      refreshBackendHealth(),
      refreshBackendConfig(),
    ]).finally(() => {
      backendRefreshPromise = null;
      backendRetryButton.disabled = false;
    });

    return backendRefreshPromise;
  }

  function renderValidationError(message, invalidElement = promptInput) {
    promptFeedback.textContent = message;
    promptFeedback.hidden = false;
    invalidElement.setAttribute('aria-invalid', 'true');
  }

  function clearValidationError() {
    promptFeedback.textContent = '';
    promptFeedback.hidden = true;
    promptInput.removeAttribute('aria-invalid');
    languageInput.removeAttribute('aria-invalid');
    codeEditor.removeAttribute('aria-invalid');
  }

  function generationErrorMessage(error) {
    if (!(error instanceof ApiError)) {
      return 'Generation failed. Check that the local backend is running and try again.';
    }

    const code = /^[A-Z0-9_]+$/.test(error.code) ? ` (${error.code})` : '';
    const message = error.message.trim() || 'The backend could not generate code.';
    return `Generation failed${code}: ${message}`;
  }

  function clearProposalForNewRequest() {
    activeProposal = null;
    renderDecisionControls();
  }

  function decisionFailureMessage(error, action) {
    const actionName = action === 'approve' ? 'Approval' : 'Rejection';
    if (error instanceof ApiError && error.status === 409) {
      return `${actionName} conflicted with the backend state (HTTP 409). The current decision is unknown.`;
    }
    if (error instanceof ApiError && error.status === 404) {
      return 'The backend no longer has this proposal (HTTP 404). Its decision state is unknown.';
    }
    if (error instanceof ApiError && error.status !== null) {
      return `${actionName} was not confirmed (HTTP ${error.status}). The proposal is no longer actionable here.`;
    }
    if (error instanceof ApiError) {
      return `${actionName} was not confirmed. ${error.message} The backend may have processed it; the proposal is disabled because its current state cannot be queried.`;
    }
    return `${actionName} was not confirmed. The backend may have processed it; the proposal is disabled because its current state cannot be queried.`;
  }

  async function submitProposalDecision(action) {
    if (workflowInProgress || activeProposal?.status !== 'PENDING') {
      return;
    }

    const proposal = activeProposal;
    setWorkflowInProgress(true, action === 'approve' ? 'Approving…' : 'Rejecting…');
    renderStatus(action === 'approve' ? 'Waiting for approval confirmation…' : 'Waiting for rejection confirmation…');

    try {
      const confirmed = action === 'approve'
        ? await approveProposal(proposal.id)
        : await rejectProposal(proposal.id);

      if (activeProposal?.id !== proposal.id) {
        return;
      }
      activeProposal = { ...proposal, status: confirmed.status };
      proposalReviewStatus.textContent = confirmed.status === 'APPROVED'
        ? 'Approved'
        : 'Rejected';
      proposalReviewAvailability.textContent = confirmed.status === 'APPROVED'
        ? 'The backend confirmed approval. This proposal has not been saved or applied.'
        : 'The backend confirmed rejection. This proposal cannot be approved or saved.';
      renderStatus(confirmed.status === 'APPROVED'
        ? 'Proposal approved. No file was saved or changed.'
        : 'Proposal rejected. No file was saved or changed.');
    } catch (error) {
      if (activeProposal?.id === proposal.id) {
        activeProposal = { ...proposal, status: 'UNKNOWN' };
      }
      const message = decisionFailureMessage(error, action);
      proposalReviewStatus.textContent = 'Decision unconfirmed';
      proposalReviewAvailability.textContent = `${message} No proposal-status lookup is available, so further decisions are disabled.`;
      renderStatus(message);
    } finally {
      setWorkflowInProgress(false);
    }
  }

  async function submitWorkflow() {
    if (workflowInProgress) {
      return;
    }

    clearValidationError();
    const instruction = promptInput.value;
    const language = languageInput.value;

    if (instruction.trim().length === 0) {
      renderValidationError(selectedMode === 'refactor'
        ? 'Enter an instruction before creating a proposal.'
        : 'Enter an instruction before generating code.');
      return;
    }
    if (language.trim().length === 0) {
      renderValidationError(
        'Enter a programming language before generating code.',
        languageInput,
      );
      return;
    }

    languageInput.removeAttribute('aria-invalid');
    const originalSource = codeEditor.value;
    if (selectedMode === 'refactor' && originalSource.trim().length === 0) {
      renderValidationError('Enter source code before creating a proposal.', codeEditor);
      return;
    }

    const requestMode = selectedMode;
    clearProposalForNewRequest();
    setWorkflowInProgress(true, requestMode === 'refactor' ? 'Creating…' : 'Generating…');
    proposalOriginalCode.textContent = originalSource;
    proposalProposedCode.textContent = requestMode === 'refactor'
      ? 'Creating proposal…'
      : 'Generating code…';
    proposalReviewStatus.textContent = requestMode === 'refactor'
      ? 'Creating proposal…'
      : 'Generating code…';
    proposalReviewAvailability.textContent = requestMode === 'refactor'
      ? 'Waiting for a confirmed backend proposal.'
      : 'Generated output cannot be approved or rejected.';
    renderStatus(requestMode === 'refactor'
      ? 'Creating a refactoring proposal with the backend…'
      : 'Generating code with the backend…');

    try {
      if (requestMode === 'refactor') {
        const proposal = await refactorCode({
          sourceCode: originalSource,
          instruction,
          language,
          editorRevision: `frontend-revision-${++editorRevisionSequence}`,
        });
        activeProposal = {
          id: proposal.proposalId,
          status: 'PENDING',
        };
        proposalOriginalCode.textContent = proposal.originalCode;
        proposalProposedCode.textContent = proposal.proposedCode;
        proposalReviewStatus.textContent = 'Proposed · PENDING';
        proposalReviewAvailability.textContent = 'This proposal is pending backend review. Approval does not save or apply it.';
        renderStatus('Refactoring proposal is ready for review. It has not been approved or saved.');
      } else {
        const response = await generateCode({ instruction, language });
        proposalOriginalCode.textContent = originalSource;
        proposalProposedCode.textContent = response.generatedCode;
        proposalReviewStatus.textContent = 'Generated output · not a proposal';
        proposalReviewAvailability.textContent = 'Generated output has no proposal ID and cannot be approved or rejected.';
        renderStatus('Generated code is displayed. It is not a proposal and has not been saved.');
      }
    } catch (error) {
      activeProposal = null;
      const message = requestMode === 'refactor'
        ? error instanceof ApiError
          ? `Proposal was not confirmed (${error.code}): ${error.message}`
          : 'Proposal was not confirmed. The backend may have processed the request, but no proposal ID was received.'
        : generationErrorMessage(error);
      proposalProposedCode.textContent = requestMode === 'refactor'
        ? 'No confirmed proposal is available.'
        : 'No generated code is available.';
      proposalReviewStatus.textContent = requestMode === 'refactor'
        ? 'Proposal unconfirmed'
        : 'Generation failed';
      proposalReviewAvailability.textContent = requestMode === 'refactor'
        ? 'No confirmed proposal ID is available, so review decisions are disabled.'
        : 'Generation did not produce reviewable proposal state.';
      promptFeedback.textContent = message;
      promptFeedback.hidden = false;
      renderStatus(message);
    } finally {
      setWorkflowInProgress(false);
    }
  }

  function selectMode(selectedButton) {
    if (workflowInProgress) {
      return;
    }
    selectedMode = selectedButton.dataset.mode;
    modeButtons.forEach((button) => {
      button.setAttribute(
        'aria-pressed',
        String(button === selectedButton),
      );
    });
    submitButtons.forEach((button) => {
      button.textContent = selectedMode === 'refactor' ? 'Create proposal' : 'Generate code';
    });
    promptInput.placeholder = selectedMode === 'refactor'
      ? 'Describe how to refactor the current code.'
      : 'Describe the code to generate.';
  }

  modeButtons.forEach((button) => {
    button.addEventListener('click', () => selectMode(button));
  });

  acceptProposalButton.addEventListener('click', () => {
    void submitProposalDecision('approve');
  });
  rejectProposalButton.addEventListener('click', () => {
    void submitProposalDecision('reject');
  });

  renderDecisionControls();

  backendRetryButton.addEventListener('click', () => {
    void refreshBackendInformation();
  });

  promptForm.addEventListener('submit', (event) => {
    event.preventDefault();
    void submitWorkflow();
  });

  void refreshBackendInformation();
  console.info('CodeAgent frontend initialized.');
}

initializeApp();
