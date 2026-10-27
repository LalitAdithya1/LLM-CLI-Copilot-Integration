import { ApiError, generateCode, getConfig, getHealth } from './api.js';

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
  '#proposal-original-code',
  '#proposal-proposed-code',
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
  const proposalOriginalCode = document.querySelector('#proposal-original-code');
  const proposalProposedCode = document.querySelector('#proposal-proposed-code');
  const generateButtons = [
    document.querySelector('#generate-submit-button'),
    document.querySelector('#generate-code-button'),
  ];
  let submissionInProgress = false;
  let generationRequestId = 0;
  let backendRefreshPromise = null;
  const originalCode = SAMPLE_ORIGINAL_CODE;
  let draftCode = originalCode;

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
  }

  function generationErrorMessage(error) {
    if (!(error instanceof ApiError)) {
      return 'Generation failed. Check that the local backend is running and try again.';
    }

    const code = /^[A-Z0-9_]+$/.test(error.code) ? ` (${error.code})` : '';
    const message = error.message.trim() || 'The backend could not generate code.';
    return `Generation failed${code}: ${message}`;
  }

  async function submitGeneration() {
    if (submissionInProgress) {
      return;
    }

    clearValidationError();
    const instruction = promptInput.value;
    const language = languageInput.value;

    if (instruction.trim().length === 0) {
      renderValidationError('Enter an instruction before generating code.');
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
    submissionInProgress = true;
    const requestId = ++generationRequestId;
    const originalSource = codeEditor.value;

    promptInput.disabled = true;
    languageInput.disabled = true;
    generateButtons.forEach((button) => {
      button.disabled = true;
      button.textContent = 'Generating…';
    });
    proposalOriginalCode.textContent = originalSource;
    proposalProposedCode.textContent = 'Generating…';
    proposalReviewStatus.textContent = 'Generating code…';
    renderStatus('Generating code with the backend…');

    try {
      const response = await generateCode({ instruction, language });
      if (requestId !== generationRequestId) {
        return;
      }

      proposalOriginalCode.textContent = originalSource;
      proposalProposedCode.textContent = response.generatedCode;
      proposalReviewStatus.textContent = 'Proposal ready for review';
      renderStatus('Generated code is ready for review. It has not been applied or saved.');
    } catch (error) {
      if (requestId !== generationRequestId) {
        return;
      }

      proposalProposedCode.textContent = 'No proposal was generated.';
      proposalReviewStatus.textContent = 'Generation failed';
      const message = generationErrorMessage(error);
      promptFeedback.textContent = message;
      promptFeedback.hidden = false;
      renderStatus(message);
    } finally {
      if (requestId === generationRequestId) {
        submissionInProgress = false;
        promptInput.disabled = false;
        languageInput.disabled = false;
        generateButtons.forEach((button) => {
          button.disabled = false;
          button.textContent = 'Generate code';
        });
      }
    }
  }

  function selectMode(selectedButton) {
    modeButtons.forEach((button) => {
      button.setAttribute(
        'aria-pressed',
        String(button === selectedButton),
      );
    });
  }

  modeButtons.forEach((button) => {
    button.addEventListener('click', () => selectMode(button));
  });

  backendRetryButton.addEventListener('click', () => {
    void refreshBackendInformation();
  });

  promptForm.addEventListener('submit', (event) => {
    event.preventDefault();
    void submitGeneration();
  });

  void refreshBackendInformation();
  console.info('CodeAgent frontend initialized.');
}

initializeApp();
