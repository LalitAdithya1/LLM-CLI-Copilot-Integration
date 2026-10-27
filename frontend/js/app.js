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
  '#prompt-feedback',
];

function initializeApp() {
  const missingElements = REQUIRED_ELEMENTS.filter(
    (selector) => document.querySelector(selector) === null,
  );

  if (missingElements.length > 0) {
    console.error(
      `CodeAgent could not initialize. Missing required elements: ${missingElements.join(', ')}`,
    );
    return;
  }

  const modeButtons = [...document.querySelectorAll('.mode-card[data-mode]')];
  const promptForm = document.querySelector('#prompt-form');
  const promptInput = document.querySelector('#prompt-input');
  const promptFeedback = document.querySelector('#prompt-feedback');
  const notificationArea = document.querySelector('#notification-area');
  let submissionInProgress = false;

  function renderStatus(message) {
    notificationArea.textContent = message;
  }

  function renderValidationError(message) {
    promptFeedback.textContent = message;
    promptFeedback.hidden = false;
    promptInput.setAttribute('aria-invalid', 'true');
  }

  function clearValidationError() {
    promptFeedback.textContent = '';
    promptFeedback.hidden = true;
    promptInput.removeAttribute('aria-invalid');
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

  promptForm.addEventListener('submit', (event) => {
    event.preventDefault();

    if (submissionInProgress) {
      return;
    }

    submissionInProgress = true;

    try {
      clearValidationError();
      const instruction = promptInput.value.trim();

      if (instruction.length === 0) {
        renderValidationError('Enter an instruction before validating.');
        return;
      }

      renderStatus(
        'Instructions are valid. Backend execution is not connected yet.',
      );
    } finally {
      submissionInProgress = false;
    }
  });

  console.info('CodeAgent frontend initialized. Backend features remain unavailable.');
}

initializeApp();
