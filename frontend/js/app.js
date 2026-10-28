import {
  ApiError,
  approveProposal,
  generateCode,
  getProposalDiff,
  getConfig,
  getHealth,
  markProposalApplied,
  openSourceFileDialog,
  readSourceFile,
  refactorCode,
  rejectProposal,
  saveGeneratedCode,
  saveProposal,
  selectOutputDirectory,
  writeSourceFile,
} from './api.js';
import { createProposalWorkflow } from './proposalWorkflow.js';
import { renderDiff } from './diffView.js';
import { createChatTranscript } from './chatTranscript.js';

const SAMPLE_ORIGINAL_CODE = `#include <iostream>

int main() {
    std::cout << "Hello, CodeAgent!\\n";
    return 0;
}
`;

// Mirrors the supported extensions of the backend, which validates saves again.
const SOURCE_FILE_EXTENSIONS = ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'];
// Keeps a loaded file and its refactoring instruction under the 1 MiB API limit.
const MAX_LOADED_FILE_BYTES = 512 * 1024;
// Name shown for editor content that is not tied to a file yet. It uses a
// supported extension so Save As can keep it.
const UNTITLED_DOCUMENT_NAME = 'untitled.cpp';

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
  '#load-source-file-button',
  '#load-source-file-input',
  '#source-path-input',
  '#open-source-path-button',
  '#output-directory-input',
  '#set-output-directory-button',
  '#file-status',
  '#copy-code-button',
  '#save-document-button',
  '#save-document-as-button',
  '#reload-document-button',
  '#document-status',
  '#conversation',
  '#conversation-empty',
  '#clear-chat-button',
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
  const loadSourceFileButton = document.querySelector('#load-source-file-button');
  const loadSourceFileInput = document.querySelector('#load-source-file-input');
  const sourcePathInput = document.querySelector('#source-path-input');
  const openSourcePathButton = document.querySelector('#open-source-path-button');
  const outputDirectoryInput = document.querySelector('#output-directory-input');
  const setOutputDirectoryButton = document.querySelector('#set-output-directory-button');
  const fileStatus = document.querySelector('#file-status');
  const copyCodeButton = document.querySelector('#copy-code-button');
  const saveDocumentButton = document.querySelector('#save-document-button');
  const saveDocumentAsButton = document.querySelector('#save-document-as-button');
  const reloadDocumentButton = document.querySelector('#reload-document-button');
  const documentStatus = document.querySelector('#document-status');
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
  const clearChatButton = document.querySelector('#clear-chat-button');
  const chat = createChatTranscript(
    document.querySelector('#conversation'),
    document.querySelector('#conversation-empty'),
  );
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
  // The document in the editor. path and revision are set only when the
  // content is tied to a file on disk that the backend can save in place;
  // the sample and browser uploads have no path.
  let currentDocument = {
    name: UNTITLED_DOCUMENT_NAME,
    path: null,
    revision: null,
    savedContent: SAMPLE_ORIGINAL_CODE,
    origin: 'sample',
  };
  let draftCode = currentDocument.savedContent;
  let documentOperationInProgress = false;
  // Latest generated (non-proposal) output: { code, saved }.
  let generatedOutput = null;
  let generatedSaveInProgress = false;
  // ID of the proposal whose code was accepted into the editor.
  let acceptedProposalId = null;

  const proposalWorkflow = createProposalWorkflow({
    getProposalDiff,
    approveProposal,
    rejectProposal,
    saveProposal,
  }, renderProposalState);

  function renderDecisionControls() {
    const state = proposalWorkflow.getState();
    const hasPendingProposal = state.proposal?.status === 'PENDING';
    const operationInProgress = Boolean(state.operation) || generatedSaveInProgress
      || Boolean(documentOperationInProgress);
    const actionLocked = workflowInProgress || operationInProgress;
    promptInput.disabled = actionLocked;
    languageInput.disabled = actionLocked;
    codeEditor.disabled = actionLocked;
    loadSourceFileButton.disabled = actionLocked;
    openSourcePathButton.disabled = actionLocked || selectionInProgress;
    selectSourceFileButton.disabled = actionLocked || selectionInProgress;
    saveDocumentButton.disabled = actionLocked || !currentDocument.path || !isDocumentDirty();
    saveDocumentAsButton.disabled = actionLocked;
    reloadDocumentButton.disabled = actionLocked || !currentDocument.path;
    saveDocumentButton.textContent = documentOperationInProgress === 'save' ? 'Saving…' : 'Save';
    modeButtons.forEach((button) => {
      button.disabled = actionLocked || button.dataset.mode === 'files';
    });
    submitButtons.forEach((button) => {
      button.disabled = actionLocked;
    });
    const canDecideProposal = hasPendingProposal && state.proposal.reviewed
      && state.proposal.sourceMatches && !state.decisionBlocked;
    const canDecideGenerated = !state.proposal && generatedOutput !== null
      && generatedOutput.decision === null;
    const canDecide = (canDecideProposal || canDecideGenerated) && !actionLocked;
    reviewChangesButton.disabled = !hasPendingProposal || workflowInProgress || operationInProgress;
    reviewChangesButton.textContent = state.operation === 'review'
      ? 'Loading diff…'
      : state.proposal?.reviewed ? 'Refresh diff' : 'Review changes';
    acceptProposalButton.disabled = !canDecide;
    rejectProposalButton.disabled = !canDecide;
    // Accepted proposals are saved from the editor (Save / Save As); only
    // generated output is written to the output directory from here.
    const canSaveGenerated = !state.proposal && generatedOutput !== null
      && !generatedOutput.saved && generatedOutput.decision !== 'rejected';
    saveFileButton.disabled = !canSaveGenerated || actionLocked;
    saveFileName.disabled = !canSaveGenerated || actionLocked;
    saveFileButton.textContent = state.operation === 'save' || generatedSaveInProgress
      ? 'Saving…'
      : 'Save output';
  }

  function renderProposalState(state = proposalWorkflow.getState()) {
    const proposal = state.proposal;
    if (!proposal) {
      proposalReviewStatus.textContent = 'No proposal available';
      proposalReviewAvailability.textContent = 'Refactoring proposals and generated code appear here for review before anything is saved.';
      proposalDiffContent.textContent = 'No diff loaded.';
      renderDecisionControls();
      return;
    }

    proposalOriginalCode.textContent = proposal.originalCode;
    proposalProposedCode.textContent = proposal.proposedCode;
    if (state.diff === null) {
      proposalDiffContent.textContent = state.operation === 'review'
        ? 'Loading diff…'
        : 'Select Review changes to load the backend diff.';
    } else {
      renderDiff(proposalDiffContent, state.diff);
    }

    const acceptedIntoEditor = proposal.status === 'APPROVED' && proposal.id === acceptedProposalId;
    const statusLabels = {
      PENDING: 'Proposal pending review',
      APPROVED: acceptedIntoEditor ? 'Accepted into the editor · not saved' : 'Approved',
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
    } else if (acceptedIntoEditor) {
      proposalReviewAvailability.textContent = `The proposed code replaced the editor contents of ${currentDocument.name}. Nothing was written to disk: review or edit it, then select Save (Ctrl+S) or Save As.`;
    } else if (!proposal.sourceMatches) {
      proposalReviewAvailability.textContent = 'The editor content has changed since this proposal was created. Restore the original source shown above before approving or saving it.';
    } else if (proposal.status === 'PENDING') {
      proposalReviewAvailability.textContent = state.decisionBlocked
        ? 'The last decision was not confirmed. No proposal-status lookup is available, so further decisions are disabled.'
        : proposal.reviewed
          ? 'Review the diff, then Accept it into the editor or Reject it.'
          : 'Load the backend diff before approving or rejecting this proposal.';
    } else if (proposal.status === 'APPROVED') {
      proposalReviewAvailability.textContent = 'The backend confirmed approval, but the proposal was not applied to the editor.';
    } else if (proposal.status === 'REJECTED') {
      proposalReviewAvailability.textContent = 'The backend confirmed rejection. The editor and files on disk are unchanged.';
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

  function isDocumentDirty() {
    return draftCode !== currentDocument.savedContent;
  }

  // Keeps the heading, footer, and dirty marker in step with currentDocument.
  function renderDocumentState() {
    const dirty = isDocumentDirty();
    previewHeading.textContent = dirty ? `${currentDocument.name} ●` : currentDocument.name;
    previewHeading.title = currentDocument.path || 'Not saved to disk';
    codeLanguage.textContent = currentDocument.path
      ? currentDocument.path
      : currentDocument.origin === 'upload'
        ? 'Opened in the browser · location unknown, use Save As to write it to disk'
        : 'New document · not saved to disk';
    codeStatus.textContent = dirty
      ? 'Unsaved changes'
      : currentDocument.path ? 'Saved to disk' : 'Not saved to disk';
  }

  function renderEditorState() {
    renderLineNumbers();
    renderDocumentState();
    renderDecisionControls();
  }

  function renderMessage(element, message, tone = 'info') {
    element.textContent = message;
    element.dataset.tone = tone;
    element.hidden = message.length === 0;
    renderStatus(message);
  }

  function setDocumentOperation(operation) {
    documentOperationInProgress = operation;
    renderDecisionControls();
  }

  function editorIsBusy() {
    return workflowInProgress || generatedSaveInProgress || documentOperationInProgress
      || Boolean(proposalWorkflow.getState().operation);
  }

  // Asks before replacing editor content that has not been saved.
  function confirmDiscardChanges(action) {
    if (!isDocumentDirty()) return true;
    return window.confirm(`${currentDocument.name} has unsaved changes. Discard them and ${action}?`);
  }

  function fileNameOf(path) {
    return path.split(/[\\/]/).filter(Boolean).pop() || path;
  }

  // Replaces the editor with a new document; any proposal for the previous
  // content no longer applies.
  function loadDocument(nextDocument) {
    proposalWorkflow.reset();
    generatedOutput = null;
    currentDocument = nextDocument;
    draftCode = nextDocument.savedContent;
    codeEditor.value = draftCode;
    proposalWorkflow.setCurrentSourceCode(draftCode);
    saveFileName.value = nextDocument.name;
    renderEditorState();
  }

  codeEditor.value = draftCode;
  proposalWorkflow.setCurrentSourceCode(draftCode);
  renderLineNumbers();
  renderDocumentState();

  codeEditor.addEventListener('input', () => {
    draftCode = codeEditor.value;
    renderEditorState();
    proposalWorkflow.setCurrentSourceCode(draftCode);
  });

  codeEditor.addEventListener('keydown', (event) => {
    if ((event.ctrlKey || event.metaKey) && String(event.key).toLowerCase() === 's') {
      event.preventDefault();
      void (currentDocument.path ? saveDocument() : saveDocumentAs());
    }
  });

  if (typeof window.addEventListener === 'function') {
    window.addEventListener('beforeunload', (event) => {
      if (isDocumentDirty() && currentDocument.origin !== 'sample') {
        event.preventDefault();
        event.returnValue = '';
      }
    });
  }

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
    selectOutputDirectoryButton.disabled = isInProgress;
    setOutputDirectoryButton.disabled = isInProgress;
    renderDecisionControls();
  }

  // Opens a file by path through the backend, which validates it and remembers
  // it so that Save can later write back to exactly this file.
  async function openSourcePath(path) {
    if (editorIsBusy()) return false;
    if (!confirmDiscardChanges(`open ${fileNameOf(path)}`)) {
      renderMessage(fileStatus, 'Opening was cancelled; the editor still has your unsaved changes.');
      return false;
    }
    setDocumentOperation('open');
    renderMessage(fileStatus, `Opening ${path}…`);
    try {
      const file = await readSourceFile(path);
      const name = fileNameOf(file.path);
      loadDocument({
        name, path: file.path, revision: file.revision, savedContent: file.content, origin: 'disk',
      });
      sourcePathInput.value = file.path;
      renderMessage(fileStatus, `Opened ${file.path}. Edit it here and select Save to write it back.`, 'success');
      return true;
    } catch (error) {
      renderMessage(fileStatus, `Could not open the file: ${apiErrorMessage(error, 'check that the local backend is running.')}`, 'error');
      return false;
    } finally {
      setDocumentOperation(false);
    }
  }

  async function openTypedSourcePath() {
    const path = sourcePathInput.value.trim();
    if (!path) {
      sourcePathInput.setAttribute('aria-invalid', 'true');
      renderMessage(fileStatus, 'Enter the full path of a source file, for example /mnt/d/project/main.cpp or D:\\project\\main.cpp.', 'error');
      sourcePathInput.focus();
      return;
    }
    sourcePathInput.removeAttribute('aria-invalid');
    await openSourcePath(path);
  }

  async function handleSourceFileSelection() {
    if (selectionInProgress || editorIsBusy()) {
      return;
    }
    setSelectionInProgress(true);
    renderMessage(fileStatus, 'Opening the file dialog… It may appear behind the browser window.');
    let selectedPath = null;
    try {
      const result = await openSourceFileDialog();
      if (result.status === 'CANCELLED') {
        renderMessage(fileStatus, 'Source-file selection cancelled. The editor is unchanged.');
        return;
      }
      if (result.status !== 'SELECTED') {
        throw new ApiError('INVALID_RESPONSE', 'The backend did not confirm a source-file selection.');
      }
      selectedPath = result.selectedPath;
    } catch (error) {
      if (error instanceof ApiError && error.code === 'DIALOG_UNAVAILABLE') {
        renderMessage(fileStatus, 'No native file dialog is available here. Type the file path below and select Open path, or use Open file in editor.', 'error');
        sourcePathInput.focus();
      } else {
        renderMessage(fileStatus, pathSelectionErrorMessage(error, 'Source-file selection'), 'error');
      }
      return;
    } finally {
      setSelectionInProgress(false);
    }
    sourcePathInput.value = selectedPath;
    await openSourcePath(selectedPath);
  }

  // Reads a file chosen in the browser file picker. Browsers do not reveal
  // where such a file lives, so it can only be written with Save As.
  async function loadSourceFileIntoEditor(file) {
    if (!file || editorIsBusy()) {
      return;
    }
    const dot = file.name.lastIndexOf('.');
    const extension = dot >= 0 ? file.name.slice(dot).toLowerCase() : '';
    if (!SOURCE_FILE_EXTENSIONS.includes(extension)) {
      renderMessage(fileStatus, `${file.name} is not a supported source file (${SOURCE_FILE_EXTENSIONS.join(', ')}).`, 'error');
      return;
    }
    if (file.size > MAX_LOADED_FILE_BYTES) {
      renderMessage(fileStatus, `${file.name} is larger than ${MAX_LOADED_FILE_BYTES / 1024} KiB and cannot be loaded.`, 'error');
      return;
    }
    if (!confirmDiscardChanges(`open ${file.name}`)) {
      renderMessage(fileStatus, 'Opening was cancelled; the editor still has your unsaved changes.');
      return;
    }
    let text;
    try {
      text = await file.text();
    } catch {
      renderMessage(fileStatus, `${file.name} could not be read.`, 'error');
      return;
    }
    loadDocument({ name: file.name, path: null, revision: null, savedContent: text, origin: 'upload' });
    renderMessage(fileStatus, `Loaded ${file.name}. The browser does not reveal its location, so use Save As to write it, or open it by path to save in place.`, 'success');
  }

  function applyOutputDirectory(result) {
    if (result.status === 'CANCELLED') {
      renderMessage(fileStatus, 'Output-directory selection cancelled. The output directory is unchanged.');
      return;
    }
    if (result.status !== 'SELECTED') {
      throw new ApiError('INVALID_RESPONSE', 'The backend did not confirm an output-directory selection.');
    }
    outputDirectorySelectionVersion += 1;
    backendConfigOutputDirectory.textContent = result.activeOutputDirectory;
    outputDirectoryInput.value = result.activeOutputDirectory;
    backendConfigValues.hidden = false;
    renderMessage(fileStatus, `Output directory set to ${result.activeOutputDirectory}.`, 'success');
  }

  async function handleOutputDirectorySelection() {
    if (selectionInProgress) {
      return;
    }
    setSelectionInProgress(true);
    renderMessage(fileStatus, 'Opening the directory dialog… It may appear behind the browser window.');
    try {
      applyOutputDirectory(await selectOutputDirectory());
    } catch (error) {
      if (error instanceof ApiError && error.code === 'DIALOG_UNAVAILABLE') {
        renderMessage(fileStatus, 'No native directory dialog is available here. Type the directory path below and select Set directory.', 'error');
        outputDirectoryInput.focus();
      } else {
        renderMessage(fileStatus, pathSelectionErrorMessage(error, 'Output-directory selection'), 'error');
      }
    } finally {
      setSelectionInProgress(false);
    }
  }

  // The backend checks that the typed path is an existing, writable directory
  // and translates Windows paths such as D:\work under WSL.
  async function setTypedOutputDirectory() {
    if (selectionInProgress) return;
    const path = outputDirectoryInput.value.trim();
    if (!path) {
      outputDirectoryInput.setAttribute('aria-invalid', 'true');
      renderMessage(fileStatus, 'Enter the full path of an existing directory.', 'error');
      outputDirectoryInput.focus();
      return;
    }
    outputDirectoryInput.removeAttribute('aria-invalid');
    setSelectionInProgress(true);
    try {
      applyOutputDirectory(await selectOutputDirectory(path));
    } catch (error) {
      outputDirectoryInput.setAttribute('aria-invalid', 'true');
      renderMessage(fileStatus, pathSelectionErrorMessage(error, 'Setting the output directory'), 'error');
    } finally {
      setSelectionInProgress(false);
    }
  }

  async function copyEditorContents() {
    const text = codeEditor.value;
    const lineCount = text.split('\n').length;
    let copied = false;
    const clipboard = globalThis.navigator?.clipboard;
    if (typeof clipboard?.writeText === 'function') {
      try {
        await clipboard.writeText(text);
        copied = true;
      } catch {
        copied = false;
      }
    }
    // The Clipboard API needs a secure context and permission; fall back to
    // copying a selection, which browsers allow during a click.
    if (!copied && typeof document.execCommand === 'function') {
      codeEditor.focus();
      codeEditor.select?.();
      try {
        copied = document.execCommand('copy');
      } catch {
        copied = false;
      }
    }
    renderMessage(documentStatus, copied
      ? `Copied ${lineCount} line${lineCount === 1 ? '' : 's'} of ${currentDocument.name} to the clipboard.`
      : 'Copy failed: clipboard access was denied. Select the editor text and press Ctrl+C instead.',
    copied ? 'success' : 'error');
  }

  async function saveDocument() {
    if (editorIsBusy() || !currentDocument.path) return false;
    if (!isDocumentDirty()) {
      renderMessage(documentStatus, `${currentDocument.name} has no unsaved changes.`);
      return true;
    }
    const savedDocument = currentDocument;
    const content = draftCode;
    setDocumentOperation('save');
    renderMessage(documentStatus, `Saving ${savedDocument.path}…`);
    try {
      let result;
      try {
        result = await writeSourceFile({
          path: savedDocument.path, content, baseRevision: savedDocument.revision, overwriteConfirmed: false,
        });
      } catch (error) {
        if (!(error instanceof ApiError) || error.code !== 'FILE_CHANGED_ON_DISK') throw error;
        if (!window.confirm(`${savedDocument.name} was changed on disk after you opened it, for example in VS Code. Overwrite it with the editor contents?\n\n${error.message}`)) {
          renderMessage(documentStatus, 'Save cancelled. The file on disk was not changed; use Reload to see its current contents.', 'error');
          return false;
        }
        result = await writeSourceFile({
          path: savedDocument.path, content, baseRevision: savedDocument.revision, overwriteConfirmed: true,
        });
      }
      currentDocument = { ...savedDocument, revision: result.revision, savedContent: content };
      renderMessage(documentStatus, `Saved ${result.savedPath}.`, 'success');
      return true;
    } catch (error) {
      renderMessage(documentStatus, `Save failed: ${apiErrorMessage(error, 'the file could not be saved.')}`, 'error');
      return false;
    } finally {
      setDocumentOperation(false);
      renderDocumentState();
    }
  }

  // Writes the editor to <active output directory>/<name>, then reopens that
  // file so later saves go to it.
  async function saveDocumentAs() {
    if (editorIsBusy()) return false;
    const outputDirectory = backendConfigOutputDirectory.textContent.trim();
    const answer = window.prompt(
      `Save as (supported: ${SOURCE_FILE_EXTENSIONS.join(' ')}).\nThe file is written to the active output directory:\n${outputDirectory || '(not loaded)'}`,
      currentDocument.name,
    );
    if (typeof answer !== 'string' || answer.trim().length === 0) {
      renderMessage(documentStatus, 'Save As cancelled.');
      return false;
    }
    const fileName = answer.trim();
    const content = draftCode;
    if (content.trim().length === 0) {
      renderMessage(documentStatus, 'The editor is empty; there is nothing to save.', 'error');
      return false;
    }
    setDocumentOperation('saveAs');
    renderMessage(documentStatus, `Saving ${fileName}…`);
    try {
      let result;
      try {
        result = await saveGeneratedCode({ fileName, content, overwriteConfirmed: false });
      } catch (error) {
        if (!(error instanceof ApiError) || error.code !== 'OVERWRITE_CONFIRMATION_REQUIRED') throw error;
        if (!window.confirm(`${fileName} already exists in ${outputDirectory}. Replace it with the editor contents?`)) {
          renderMessage(documentStatus, 'Save As cancelled. The existing file was not changed.');
          return false;
        }
        result = await saveGeneratedCode({ fileName, content, overwriteConfirmed: true });
      }
      let file = null;
      try {
        file = await readSourceFile(result.savedPath);
      } catch {
        file = null;
      }
      currentDocument = file
        ? { name: fileNameOf(file.path), path: file.path, revision: file.revision, savedContent: content, origin: 'disk' }
        : { name: fileName, path: null, revision: null, savedContent: content, origin: 'upload' };
      saveFileName.value = currentDocument.name;
      renderMessage(documentStatus, `Saved ${result.savedPath}.`, 'success');
      return true;
    } catch (error) {
      renderMessage(documentStatus, `Save As failed: ${apiErrorMessage(error, 'the file could not be saved.')}`, 'error');
      return false;
    } finally {
      setDocumentOperation(false);
      renderDocumentState();
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
        if (!outputDirectoryInput.value) outputDirectoryInput.value = config.activeOutputDirectory;
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
    return `Generation failed${code}: ${message}${providerFailureHint(error)}`;
  }

  // The backend deliberately returns generic provider errors; the specific
  // cause (missing key, HTTP status, network) is printed in its terminal.
  function providerFailureHint(error) {
    return error instanceof ApiError && ['GENERATION_FAILED', 'REFACTOR_FAILED'].includes(error.code)
      ? ' Check the terminal running codeagent for details, such as a missing GEMINI_API_KEY.'
      : '';
  }

  // Puts accepted code into the editor buffer. The document keeps its name
  // and path, so the change shows as unsaved until the user saves it.
  function applyCodeToEditor(code) {
    draftCode = code;
    codeEditor.value = code;
    proposalWorkflow.setCurrentSourceCode(code);
    renderEditorState();
  }

  function confirmReplaceEditor() {
    return !isDocumentDirty() || window.confirm(
      `${currentDocument.name} has unsaved changes. Replace the editor contents with the accepted code?`,
    );
  }

  function decideGeneratedOutput(action) {
    const output = generatedOutput;
    if (!output || output.decision !== null) return;
    if (action === 'approve') {
      if (!confirmReplaceEditor()) {
        renderStatus('Accept cancelled. The editor is unchanged.');
        return;
      }
      output.decision = 'accepted';
      applyCodeToEditor(output.code);
      chat.addNote('Generated code accepted into the editor · not saved', 'success');
      proposalReviewStatus.textContent = 'Accepted into the editor · not saved';
      proposalReviewAvailability.textContent = `The generated code replaced the editor contents of ${currentDocument.name}. Nothing was written to disk: edit it, then select Save (Ctrl+S) or Save As.`;
      renderStatus('Generated code accepted into the editor. It has not been saved.');
    } else {
      output.decision = 'rejected';
      chat.addNote('Generated code rejected');
      proposalReviewStatus.textContent = 'Generated output rejected';
      proposalReviewAvailability.textContent = 'The generated code was discarded. The editor and files on disk are unchanged.';
      renderStatus('Generated code rejected. The editor is unchanged.');
      renderDecisionControls();
    }
  }

  async function submitProposalDecision(action) {
    if (editorIsBusy()) return;
    const proposal = proposalWorkflow.getState().proposal;
    if (!proposal) {
      decideGeneratedOutput(action);
      return;
    }
    if (proposal.status !== 'PENDING') {
      return;
    }
    if (action === 'approve' && !confirmReplaceEditor()) {
      renderStatus('Accept cancelled. The editor and the proposal are unchanged.');
      return;
    }
    renderStatus(action === 'approve' ? 'Waiting for approval confirmation…' : 'Waiting for rejection confirmation…');
    const confirmed = await proposalWorkflow.decide(action);
    const current = proposalWorkflow.getState();
    if (!confirmed) {
      renderStatus(`${action === 'approve' ? 'Approval' : 'Rejection'} was not confirmed. ${current.error}`);
      return;
    }
    if (action === 'reject') {
      chat.addNote('Proposal rejected');
      renderStatus('Proposal rejected by the backend. The editor and files on disk are unchanged.');
      return;
    }
    // Apply only the proposal that was reviewed, and only onto the text it
    // was created from; the editor is locked while the approval is pending.
    if (current.proposal?.id !== proposal.id || current.proposal.status !== 'APPROVED'
      || codeEditor.value !== proposal.originalCode) {
      renderStatus('The proposal was approved, but the editor changed meanwhile, so it was not applied.');
      return;
    }
    acceptedProposalId = proposal.id;
    applyCodeToEditor(current.proposal.proposedCode);
    chat.addNote('Proposal accepted into the editor · not saved', 'success');
    renderStatus('Proposal accepted into the editor. It has not been saved: review or edit it, then Save (Ctrl+S) or Save As.');
    // Only now that the code is in the editor, let the backend release the
    // proposal. A failure here leaves the editor as is; the backend just keeps
    // the proposal until it restarts.
    try {
      await markProposalApplied(proposal.id);
    } catch (error) {
      renderStatus(`Proposal accepted into the editor and not saved. The backend could not record this (${apiErrorMessage(error, 'request failed')}); it keeps the proposal until it restarts.`);
    }
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

  async function saveGeneratedOutput() {
    const output = generatedOutput;
    if (workflowInProgress || generatedSaveInProgress || !output || output.saved) return;
    const fileName = saveFileName.value.trim();
    if (!fileName) {
      saveFileName.setAttribute('aria-invalid', 'true');
      renderStatus('Enter a filename for the generated code.');
      saveFileName.focus();
      return;
    }
    saveFileName.removeAttribute('aria-invalid');
    generatedSaveInProgress = true;
    renderDecisionControls();
    renderStatus('Saving the generated code through the backend…');
    try {
      let result;
      try {
        result = await saveGeneratedCode({ fileName, content: output.code, overwriteConfirmed: false });
      } catch (error) {
        if (!(error instanceof ApiError) || error.code !== 'OVERWRITE_CONFIRMATION_REQUIRED') throw error;
        if (!window.confirm(`${fileName} already exists in the output directory. Replace it with the generated code?`)) {
          renderStatus('Overwrite was not confirmed. The existing file was not changed.');
          return;
        }
        result = await saveGeneratedCode({ fileName, content: output.code, overwriteConfirmed: true });
      }
      output.saved = true;
      chat.addNote(`Generated code saved to ${result.savedPath}`, 'success');
      proposalReviewStatus.textContent = 'Generated code saved';
      proposalReviewAvailability.textContent = `The backend confirmed the saved file: ${result.savedPath}`;
      renderStatus(`The backend confirmed the save: ${result.savedPath}`);
    } catch (error) {
      renderStatus(`Save failed: ${apiErrorMessage(error, 'The generated code could not be saved.')}`);
    } finally {
      generatedSaveInProgress = false;
      renderDecisionControls();
    }
  }

  async function submitWorkflow() {
    if (editorIsBusy()) {
      return;
    }

    const currentProposal = proposalWorkflow.getState().proposal;
    if (currentProposal?.status === 'PENDING') {
      renderStatus('Review and accept or reject the current proposal before starting another request.');
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
    chat.addUserMessage({ mode: requestMode, language, instruction });
    const reply = chat.addPendingReply(requestMode === 'refactor'
      ? 'Thinking about the refactoring…'
      : 'Thinking about the code…');
    generatedOutput = null;
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
      : 'Waiting for generated code.';
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
        reply.resolve({
          text: `Proposed a refactoring (${describeLines(proposal.proposedCode)}). Review the diff below, then Accept or Reject it.`,
          code: proposal.proposedCode,
          thoughts: proposal.thoughts ?? '',
        });
        promptInput.value = '';
        renderStatus('Refactoring proposal is ready for review. It has not been approved or saved.');
      } else {
        const response = await generateCode({ instruction, language });
        proposalOriginalCode.textContent = originalSource;
        proposalProposedCode.textContent = response.generatedCode;
        generatedOutput = { code: response.generatedCode, saved: false, decision: null };
        reply.resolve({
          text: `Generated ${describeLines(response.generatedCode)} of ${response.language}. Accept it into the editor or Save output below.`,
          code: response.generatedCode,
          thoughts: response.thoughts ?? '',
        });
        promptInput.value = '';
        proposalReviewStatus.textContent = 'Generated output · not saved';
        proposalReviewAvailability.textContent = 'Accept puts the generated code into the editor for you to edit and Save. Save output instead writes it as-is to the output directory; existing files are replaced only after you confirm.';
        renderStatus('Generated code is displayed. It has not been saved.');
      }
    } catch (error) {
      const message = requestMode === 'refactor'
        ? error instanceof ApiError
          ? `Proposal was not confirmed (${error.code}): ${error.message}${providerFailureHint(error)}`
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
      reply.fail(message);
      promptFeedback.textContent = message;
      promptFeedback.hidden = false;
      renderStatus(message);
    } finally {
      setWorkflowInProgress(false);
    }
  }

  function describeLines(code) {
    const lineCount = code.replace(/\n$/, '').split('\n').length;
    return `${lineCount} line${lineCount === 1 ? '' : 's'}`;
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
      ? 'Describe how to refactor the current code. Enter sends, Shift+Enter adds a line.'
      : 'Describe the code to generate. Enter sends, Shift+Enter adds a line.';
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
    void saveGeneratedOutput();
  });

  selectSourceFileButton.addEventListener('click', () => {
    void handleSourceFileSelection();
  });
  loadSourceFileButton.addEventListener('click', () => {
    loadSourceFileInput.click();
  });
  openSourcePathButton.addEventListener('click', () => {
    void openTypedSourcePath();
  });
  sourcePathInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      void openTypedSourcePath();
    }
  });
  setOutputDirectoryButton.addEventListener('click', () => {
    void setTypedOutputDirectory();
  });
  outputDirectoryInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      void setTypedOutputDirectory();
    }
  });
  copyCodeButton.addEventListener('click', () => {
    void copyEditorContents();
  });
  saveDocumentButton.addEventListener('click', () => {
    void saveDocument();
  });
  saveDocumentAsButton.addEventListener('click', () => {
    void saveDocumentAs();
  });
  reloadDocumentButton.addEventListener('click', () => {
    if (currentDocument.path) void openSourcePath(currentDocument.path);
  });
  loadSourceFileInput.addEventListener('change', () => {
    const file = loadSourceFileInput.files?.[0];
    loadSourceFileInput.value = '';
    void loadSourceFileIntoEditor(file);
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

  promptInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      void submitWorkflow();
    }
  });

  clearChatButton.addEventListener('click', () => {
    chat.clear();
  });

  void refreshBackendInformation();
  console.info('CodeAgent frontend initialized.');
}

initializeApp();
