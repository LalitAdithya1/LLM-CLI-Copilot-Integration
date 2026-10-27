import {
  ApiError,
  approveProposal,
  generateCode,
  getProposalDiff,
  getConfig,
  getHealth,
  openSourceFileDialog,
  refactorCode,
  rejectProposal,
  saveProposal,
  selectOutputDirectory,
} from './api.js';
import { createProposalWorkflow } from './proposalWorkflow.js';

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
  '#select-output-directory-button',
  '#backend-config-error',
  '#generate-submit-button',
  '#generate-code-button',
  '#proposal-review-status',
  '#proposal-review-availability',
  '#proposal-original-code',
  '#proposal-proposed-code',
  '#proposal-diff-content',
  '#review-changes-button',
  '#accept-proposal-button',
  '#reject-proposal-button',
  '#save-file-button',
  '#save-file-name',
  '#preview-heading',
  '#code-language',
  '#select-source-file-button',
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
  const selectOutputDirectoryButton = document.querySelector('#select-output-directory-button');
  const backendConfigError = document.querySelector('#backend-config-error');
  const previewHeading = document.querySelector('#preview-heading');
  const codeLanguage = document.querySelector('#code-language');
  const selectSourceFileButton = document.querySelector('#select-source-file-button');
  const proposalReviewStatus = document.querySelector('#proposal-review-status');
  const proposalReviewAvailability = document.querySelector('#proposal-review-availability');
  const proposalOriginalCode = document.querySelector('#proposal-original-code');
  const proposalProposedCode = document.querySelector('#proposal-proposed-code');
  const proposalDiffContent = document.querySelector('#proposal-diff-content');
  const reviewChangesButton = document.querySelector('#review-changes-button');
  const acceptProposalButton = document.querySelector('#accept-proposal-button');
  const rejectProposalButton = document.querySelector('#reject-proposal-button');
  const saveFileButton = document.querySelector('#save-file-button');
  const saveFileName = document.querySelector('#save-file-name');
  const submitButtons = [
    document.querySelector('#generate-submit-button'),
    document.querySelector('#generate-code-button'),
  ];
  let workflowInProgress = false;
  let selectionInProgress = false;
  let outputDirectorySelectionVersion = 0;
  let backendRefreshPromise = null;
  let editorRevisionSequence = 0;
  let selectedMode = 'generate';
  const originalCode = SAMPLE_ORIGINAL_CODE;
  let draftCode = originalCode;

  const proposalWorkflow = createProposalWorkflow({
    getProposalDiff,
    approveProposal,
    rejectProposal,
    saveProposal,
  }, renderProposalState);

  function renderDecisionControls() {
    const state = proposalWorkflow.getState();
    const hasPendingProposal = state.proposal?.status === 'PENDING';
    const operationInProgress = Boolean(state.operation);
    const actionLocked = workflowInProgress || operationInProgress;
    promptInput.disabled = actionLocked;
    languageInput.disabled = actionLocked;
    codeEditor.disabled = actionLocked;
    modeButtons.forEach((button) => {
      button.disabled = actionLocked || button.dataset.mode === 'files';
    });
    submitButtons.forEach((button) => {
      button.disabled = actionLocked;
    });
    const canDecide = hasPendingProposal && state.proposal.reviewed
      && state.proposal.sourceMatches && !state.decisionBlocked
      && !workflowInProgress && !operationInProgress;
    reviewChangesButton.disabled = !hasPendingProposal || workflowInProgress || operationInProgress;
    reviewChangesButton.textContent = state.operation === 'review'
      ? 'Loading diff…'
      : state.proposal?.reviewed ? 'Refresh diff' : 'Review changes';
    acceptProposalButton.disabled = !canDecide;
    rejectProposalButton.disabled = !canDecide;
    saveFileButton.disabled = state.proposal?.status !== 'APPROVED'
      || !state.proposal?.sourceMatches
      || workflowInProgress || operationInProgress;
    saveFileName.disabled = state.proposal?.status !== 'APPROVED'
      || !state.proposal?.sourceMatches
      || workflowInProgress || operationInProgress;
    saveFileButton.textContent = state.operation === 'save' ? 'Saving…' : 'Save file';
  }

  function renderProposalState(state = proposalWorkflow.getState()) {
    const proposal = state.proposal;
    if (!proposal) {
      proposalReviewStatus.textContent = 'No proposal available';
      proposalReviewAvailability.textContent = 'Generated code is not a proposal and cannot be approved or saved.';
      proposalDiffContent.textContent = 'No diff loaded.';
      renderDecisionControls();
      return;
    }

    proposalOriginalCode.textContent = proposal.originalCode;
    proposalProposedCode.textContent = proposal.proposedCode;
    proposalDiffContent.textContent = state.diff === null
      ? state.operation === 'review' ? 'Loading diff…' : 'Select Review changes to load the backend diff.'
      : state.diff.length > 0 ? state.diff : 'No differences were reported.';

    const statusLabels = {
      PENDING: 'Proposal pending review',
      APPROVED: 'Approved · not saved',
      REJECTED: 'Rejected',
      APPLIED: 'Saved and applied',
    };
    proposalReviewStatus.textContent = state.operation === 'review'
      ? 'Loading proposal diff…'
      : state.operation === 'approve'
        ? 'Waiting for approval confirmation…'
        : state.operation === 'reject'
          ? 'Waiting for rejection confirmation…'
          : state.operation === 'save'
            ? 'Saving approved proposal…'
            : statusLabels[proposal.status] || 'Proposal status unavailable';
    if (state.savedPath) {
      proposalReviewAvailability.textContent = `The backend confirmed the saved file: ${state.savedPath}`;
    } else if (state.error) {
      proposalReviewAvailability.textContent = state.error;
    } else if (!proposal.sourceMatches) {
      proposalReviewAvailability.textContent = 'The editor content has changed since this proposal was created. Restore the original source shown above before approving or saving it.';
    } else if (proposal.status === 'PENDING') {
      proposalReviewAvailability.textContent = state.decisionBlocked
        ? 'The last decision was not confirmed. No proposal-status lookup is available, so further decisions are disabled.'
        : proposal.reviewed
          ? 'Review the diff, then approve or reject this pending proposal.'
          : 'Load the backend diff before approving or rejecting this proposal.';
    } else if (proposal.status === 'APPROVED') {
      proposalReviewAvailability.textContent = 'The backend confirmed approval. Choose a filename and save this proposal.';
    } else if (proposal.status === 'REJECTED') {
      proposalReviewAvailability.textContent = 'The backend confirmed rejection. This proposal cannot be saved.';
    } else {
      proposalReviewAvailability.textContent = 'The backend confirmed the save. This proposal cannot be saved again.';
    }
    renderDecisionControls();
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
  proposalWorkflow.setCurrentSourceCode(originalCode);
  renderEditorState();

  codeEditor.addEventListener('input', () => {
    draftCode = codeEditor.value;
    renderEditorState();
    proposalWorkflow.setCurrentSourceCode(draftCode);
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

  function pathSelectionErrorMessage(error, selectionLabel) {
    if (!(error instanceof ApiError)) {
      return `${selectionLabel} failed. Check that the local backend is running.`;
    }
    if (error.code === 'DIALOG_UNAVAILABLE') {
      return `${selectionLabel} dialog is unavailable: ${error.message}`;
    }
    const code = /^[A-Z0-9_]+$/.test(error.code) ? ` (${error.code})` : '';
    return `${selectionLabel} failed${code}: ${error.message}`;
  }

  function setSelectionInProgress(isInProgress) {
    selectionInProgress = isInProgress;
    selectSourceFileButton.disabled = isInProgress;
    selectOutputDirectoryButton.disabled = isInProgress;
  }

  async function handleSourceFileSelection() {
    if (selectionInProgress) {
      return;
    }
    setSelectionInProgress(true);
    renderStatus('Opening the native source-file dialog…');
    try {
      const result = await openSourceFileDialog();
      if (result.status === 'CANCELLED') {
        renderStatus('Source-file selection cancelled. The displayed path is unchanged.');
        return;
      }
      if (result.status !== 'SELECTED') {
        throw new ApiError('INVALID_RESPONSE', 'The backend did not confirm a source-file selection.');
      }
      previewHeading.textContent = result.selectedPath;
      previewHeading.title = result.selectedPath;
      codeLanguage.textContent = 'Path selected only · file contents not loaded';
      renderStatus('Source path selected. The file contents were not read; the editor remains unchanged.');
    } catch (error) {
      renderStatus(pathSelectionErrorMessage(error, 'Source-file selection'));
    } finally {
      setSelectionInProgress(false);
    }
  }

  async function handleOutputDirectorySelection() {
    if (selectionInProgress) {
      return;
    }
    setSelectionInProgress(true);
    renderStatus('Opening the native output-directory dialog…');
    try {
      const result = await selectOutputDirectory();
      if (result.status === 'CANCELLED') {
        renderStatus('Output-directory selection cancelled. The displayed directory is unchanged.');
        return;
      }
      if (result.status !== 'SELECTED') {
        throw new ApiError('INVALID_RESPONSE', 'The backend did not confirm an output-directory selection.');
      }
      outputDirectorySelectionVersion += 1;
      backendConfigOutputDirectory.textContent = result.activeOutputDirectory;
      backendConfigValues.hidden = false;
      renderStatus('Output directory selection confirmed by the backend.');
    } catch (error) {
      renderStatus(pathSelectionErrorMessage(error, 'Output-directory selection'));
    } finally {
      setSelectionInProgress(false);
    }
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
    const selectionVersionAtRequest = outputDirectorySelectionVersion;
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
      if (selectionVersionAtRequest === outputDirectorySelectionVersion) {
        backendConfigOutputDirectory.textContent = config.activeOutputDirectory.trim()
          ? config.activeOutputDirectory
          : 'No output directory reported';
      }
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

  async function submitProposalDecision(action) {
    const proposal = proposalWorkflow.getState().proposal;
    if (workflowInProgress || proposal?.status !== 'PENDING') {
      return;
    }
    renderStatus(action === 'approve' ? 'Waiting for approval confirmation…' : 'Waiting for rejection confirmation…');
    const confirmed = await proposalWorkflow.decide(action);
    const current = proposalWorkflow.getState();
    renderStatus(confirmed
      ? action === 'approve'
        ? 'Proposal approved by the backend. No file has been saved yet.'
        : 'Proposal rejected by the backend. It cannot be saved.'
      : `${action === 'approve' ? 'Approval' : 'Rejection'} was not confirmed. ${current.error}`);
  }

  async function reviewProposalChanges() {
    if (workflowInProgress || proposalWorkflow.getState().operation) return;
    renderStatus('Requesting the proposal diff from the backend…');
    const confirmed = await proposalWorkflow.review();
    const state = proposalWorkflow.getState();
    renderStatus(confirmed
      ? 'The backend diff is ready. Review it before approving or rejecting.'
      : `Diff review failed. ${state.error}`);
  }

  async function saveApprovedProposal() {
    const state = proposalWorkflow.getState();
    if (workflowInProgress || state.proposal?.status !== 'APPROVED' || state.operation) return;
    if (!state.proposal.sourceMatches) {
      renderStatus('The editor has changed since this proposal was created. Restore the original source before saving.');
      return;
    }
    const fileName = saveFileName.value.trim();
    if (!fileName) {
      saveFileName.setAttribute('aria-invalid', 'true');
      renderStatus('Enter a filename for the approved proposal.');
      saveFileName.focus();
      return;
    }
    saveFileName.removeAttribute('aria-invalid');
    renderStatus('Saving the approved proposal through the backend…');
    const saved = await proposalWorkflow.save(
      fileName,
      async (name) => window.confirm(
        `The target file may already exist. Replace ${name}? The backend will validate the target again before writing.`,
      ),
    );
    const result = proposalWorkflow.getState();
    renderStatus(saved
      ? `The backend confirmed the save: ${result.savedPath}`
      : result.error || 'The save was not confirmed. The approved proposal remains available to retry.');
  }

  async function submitWorkflow() {
    if (workflowInProgress || proposalWorkflow.getState().operation) {
      return;
    }

    const currentProposal = proposalWorkflow.getState().proposal;
    if (currentProposal && ['PENDING', 'APPROVED'].includes(currentProposal.status)) {
      renderStatus(currentProposal.status === 'PENDING'
        ? 'Review and approve or reject the current proposal before starting another request.'
        : 'Save the approved proposal before starting another request.');
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
    proposalWorkflow.reset();
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
        proposalWorkflow.setProposal(proposal);
        renderStatus('Refactoring proposal is ready for review. It has not been approved or saved.');
      } else {
        const response = await generateCode({ instruction, language });
        proposalOriginalCode.textContent = originalSource;
        proposalProposedCode.textContent = response.generatedCode;
        proposalReviewStatus.textContent = 'Generated output · not a proposal';
        proposalReviewAvailability.textContent = 'Generated output has no proposal ID and cannot be approved or saved through the approved-proposal workflow.';
        renderStatus('Generated code is displayed. It is not a proposal and has not been saved.');
      }
    } catch (error) {
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
      proposalDiffContent.textContent = 'No diff loaded.';
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
    if (workflowInProgress || proposalWorkflow.getState().operation) {
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
  reviewChangesButton.addEventListener('click', () => {
    void reviewProposalChanges();
  });
  saveFileButton.addEventListener('click', () => {
    void saveApprovedProposal();
  });

  selectSourceFileButton.addEventListener('click', () => {
    void handleSourceFileSelection();
  });
  selectOutputDirectoryButton.addEventListener('click', () => {
    void handleOutputDirectorySelection();
  });

  renderProposalState();

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
