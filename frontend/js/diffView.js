// Classifies and renders unified diffs with Git-style colors. Every line is
// inserted as a text node, so diff content is never interpreted as HTML.

const HUNK_HEADER = /^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@/;

// Returns [{ kind, text }] where kind is one of: 'file-header', 'hunk',
// 'removed', 'added', 'context', 'meta'. A hunk header's line counts decide
// which lines belong to the hunk, so a removed line whose code starts with
// "--" is not mistaken for a "---" file header.
export function classifyDiffLines(diffText) {
  if (typeof diffText !== 'string' || diffText.length === 0) return [];
  const lines = diffText.split('\n');
  if (lines.at(-1) === '') lines.pop();

  let oldRemaining = 0;
  let newRemaining = 0;
  let inUncountedHunk = false;
  return lines.map((text) => {
    const inHunk = oldRemaining > 0 || newRemaining > 0 || inUncountedHunk;
    if (text.startsWith('\\')) return { kind: 'meta', text };
    if (inHunk && !inUncountedHunk) {
      if (text.startsWith('-') && oldRemaining > 0) {
        oldRemaining -= 1;
        return { kind: 'removed', text };
      }
      if (text.startsWith('+') && newRemaining > 0) {
        newRemaining -= 1;
        return { kind: 'added', text };
      }
      if ((text.startsWith(' ') || text === '') && oldRemaining > 0 && newRemaining > 0) {
        oldRemaining -= 1;
        newRemaining -= 1;
        return { kind: 'context', text };
      }
    }

    const hunk = HUNK_HEADER.exec(text);
    if (hunk) {
      // An omitted count means one line (git diff convention).
      oldRemaining = hunk[1] === undefined ? 1 : Number(hunk[1]);
      newRemaining = hunk[2] === undefined ? 1 : Number(hunk[2]);
      inUncountedHunk = false;
      return { kind: 'hunk', text };
    }
    if (text.startsWith('@@')) {
      // Malformed hunk header: fall back to classifying by prefix.
      oldRemaining = 0;
      newRemaining = 0;
      inUncountedHunk = true;
      return { kind: 'hunk', text };
    }
    if (!inUncountedHunk && (text.startsWith('--- ') || text.startsWith('+++ ')
      || text.startsWith('diff ') || text.startsWith('index '))) {
      return { kind: 'file-header', text };
    }
    if (inUncountedHunk) {
      if (text.startsWith('-')) return { kind: 'removed', text };
      if (text.startsWith('+')) return { kind: 'added', text };
      if (text.startsWith(' ') || text === '') return { kind: 'context', text };
    }
    return { kind: 'meta', text };
  });
}

// Replaces container's content with the colored diff, or with message when
// there is no diff to show.
export function renderDiff(container, diffText, doc = globalThis.document) {
  const lines = classifyDiffLines(diffText);
  if (lines.length === 0) {
    container.textContent = 'No differences were reported.';
    return lines;
  }
  const fragment = doc.createDocumentFragment();
  lines.forEach(({ kind, text }, index) => {
    const line = doc.createElement('span');
    line.className = `diff-line diff-line--${kind}`;
    line.textContent = text;
    fragment.append(line);
    if (index < lines.length - 1) fragment.append(doc.createTextNode('\n'));
  });
  container.replaceChildren(fragment);
  return lines;
}
