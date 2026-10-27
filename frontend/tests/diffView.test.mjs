import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = readFileSync(new URL('../js/diffView.js', import.meta.url), 'utf8');
const { classifyDiffLines, renderDiff } = await import(
  `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`
);

const kinds = (text) => classifyDiffLines(text).map((line) => line.kind);

test('classifies headers, hunks, removed, added, and context lines like git', () => {
  const diff = [
    '--- original',
    '+++ proposed',
    '@@ -1,3 +1,3 @@',
    ' #include <iostream>',
    '-int old();',
    '+int fresh();',
    ' int main() {}',
    '',
  ].join('\n');
  assert.deepEqual(kinds(diff), [
    'file-header', 'file-header', 'hunk', 'context', 'removed', 'added', 'context',
  ]);
  assert.deepEqual(classifyDiffLines(diff)[4], { kind: 'removed', text: '-int old();' });
});

test('code lines that look like file headers stay removed or added inside a hunk', () => {
  const diff = [
    '--- original',
    '+++ proposed',
    '@@ -1,2 +1,2 @@',
    '--- x;       // removed line whose code is "-- x;"',
    '+++ y;       // added line whose code is "++ y;"',
    '-a',
    '+b',
    '',
  ].join('\n');
  assert.deepEqual(kinds(diff), ['file-header', 'file-header', 'hunk', 'removed', 'added', 'removed', 'added']);
});

test('handles no-newline markers, omitted hunk counts, and multiple hunks', () => {
  assert.deepEqual(kinds('--- a\n+++ b\n@@ -1 +1 @@\n-x\n\\ No newline at end of file\n+y\n'), [
    'file-header', 'file-header', 'hunk', 'removed', 'meta', 'added',
  ]);
  assert.deepEqual(kinds('@@ -1,1 +1,1 @@\n-a\n+b\n@@ -9,2 +9,1 @@\n c\n-d\n'), [
    'hunk', 'removed', 'added', 'hunk', 'context', 'removed',
  ]);
  assert.deepEqual(kinds('@@ -0,0 +1,2 @@\n+one\n+two\n'), ['hunk', 'added', 'added']);
});

test('empty, missing, and malformed diffs degrade gracefully', () => {
  assert.deepEqual(classifyDiffLines(''), []);
  assert.deepEqual(classifyDiffLines(null), []);
  assert.deepEqual(classifyDiffLines(undefined), []);
  assert.deepEqual(kinds('No differences.\n'), ['meta']);
  assert.deepEqual(kinds('@@ garbage @@\n-a\n+b\n c\n'), ['hunk', 'removed', 'added', 'context']);
  assert.deepEqual(kinds('random text\n-not in a hunk\n'), ['meta', 'meta']);
});

function fakeDocument() {
  const make = (tag) => ({
    tag, className: '', textContent: '', children: [],
    set innerHTML(_) { throw new Error('innerHTML must not be used'); },
  });
  return {
    createElement: make,
    createTextNode: (text) => ({ tag: '#text', textContent: text }),
    createDocumentFragment: () => ({ children: [], append(node) { this.children.push(node); } }),
  };
}

test('renders each line as a classed text-only span without interpreting HTML', () => {
  const container = { textContent: '', replaceChildren(fragment) { this.children = fragment.children; } };
  const diff = '--- original\n+++ proposed\n@@ -1,1 +1,1 @@\n-<img src=x onerror=alert(1)>\n+<script>bad()</script>\n';
  renderDiff(container, diff, fakeDocument());
  const spans = container.children.filter((node) => node.tag === 'span');
  assert.deepEqual(spans.map((span) => span.className), [
    'diff-line diff-line--file-header',
    'diff-line diff-line--file-header',
    'diff-line diff-line--hunk',
    'diff-line diff-line--removed',
    'diff-line diff-line--added',
  ]);
  assert.equal(spans[3].textContent, '-<img src=x onerror=alert(1)>');
  assert.equal(spans[4].textContent, '+<script>bad()</script>');
  assert.equal(container.children.filter((node) => node.tag === '#text').length, 4, 'newlines between lines');
  assert.equal(container.children.map((node) => node.textContent).join(''), diff.slice(0, -1),
    'the rendered text is exactly the diff, prefixes included');
});

test('an empty diff renders a message instead of an empty panel', () => {
  const container = { textContent: '', replaceChildren() { throw new Error('not expected'); } };
  renderDiff(container, '', fakeDocument());
  assert.equal(container.textContent, 'No differences were reported.');
});
