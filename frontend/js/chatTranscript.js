// Renders the assistant conversation: user prompts, Gemini replies with
// their thought summaries, and short notes about what happened to the output.
// All text is set through textContent; model output is never parsed as HTML.

const MODE_LABELS = { generate: 'Generate', refactor: 'Refactor' };

function timeLabel(date = new Date()) {
  const pad = (value) => String(value).padStart(2, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

export function createChatTranscript(container, emptyState, doc = document) {
  function element(tag, className, text) {
    const node = doc.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function scrollToLatest() {
    container.scrollTop = container.scrollHeight ?? 0;
  }

  function append(node) {
    emptyState.hidden = true;
    container.append(node);
    scrollToLatest();
    return node;
  }

  function label(name, details) {
    return element('p', 'message-label', [name, ...details, timeLabel()].join(' · '));
  }

  // Thought summaries are Markdown-like; lines that are a single **bold**
  // phrase become headings and the rest stay plain text.
  function renderThoughts(thoughts) {
    const details = element('details', 'message-thoughts');
    details.append(element('summary', 'message-thoughts__summary', 'Thoughts'));
    const body = element('div', 'message-thoughts__body');
    let paragraph = [];
    const flush = () => {
      if (paragraph.length > 0) body.append(element('p', '', paragraph.join('\n')));
      paragraph = [];
    };
    for (const line of thoughts.trim().split('\n')) {
      const heading = /^\s*\*\*(.+?)\*\*\s*$/.exec(line);
      if (heading) {
        flush();
        body.append(element('p', 'message-thoughts__heading', heading[1]));
      } else if (line.trim().length === 0) {
        flush();
      } else {
        paragraph.push(line);
      }
    }
    flush();
    details.append(body);
    return details;
  }

  function addUserMessage({ mode, language, instruction }) {
    const card = element('article', 'message-card message-card--user');
    card.append(
      label('YOU', [MODE_LABELS[mode] || mode, language.trim()]),
      element('p', 'message-text', instruction.trim()),
    );
    return append(card);
  }

  // Shows a waiting reply and returns callbacks that fill it in once.
  function addPendingReply(waitingText) {
    const card = element('article', 'message-card message-card--copilot');
    card.dataset.state = 'pending';
    const status = element('p', 'message-text message-text--pending', waitingText);
    card.append(label('GEMINI', []), status);
    append(card);
    let settled = false;

    function settle(nodes, state) {
      if (settled) return;
      settled = true;
      card.dataset.state = state;
      card.replaceChildren(label('GEMINI', []), ...nodes);
      scrollToLatest();
    }

    return {
      resolve({ text, code = '', thoughts = '' }) {
        const nodes = [];
        if (thoughts.trim().length > 0) nodes.push(renderThoughts(thoughts));
        nodes.push(element('p', 'message-text', text));
        if (code.length > 0) nodes.push(element('pre', 'message-code', code));
        settle(nodes, 'done');
      },
      fail(message) {
        settle([element('p', 'message-text message-text--error', message)], 'error');
      },
    };
  }

  function addNote(text, tone = 'info') {
    const note = element('p', 'chat-note', `${text} · ${timeLabel()}`);
    note.dataset.tone = tone;
    return append(note);
  }

  function clear() {
    container.replaceChildren(emptyState);
    emptyState.hidden = false;
  }

  return { addUserMessage, addPendingReply, addNote, clear };
}
