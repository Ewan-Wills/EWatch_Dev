import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { formatCurrentTool, projectLabel, stripNonAscii } from '../src/privacy';

test('Bash: first 24 chars of first line only, no later lines', () => {
  const out = formatCurrentTool('Bash', { command: '0123456789012345678901234567890 rest\nSECRET_SECOND_LINE' });
  // 'Bash|' (5 chars) leaves a 19-char budget out of the 24-char first-line slice.
  assert.equal(out, 'Bash|0123456789012345678');
  assert.equal(out.length, 24);
  assert.ok(!out.includes('SECRET'));
});

test('Edit/Write/Read/NotebookEdit: basename only, no path leakage', () => {
  for (const tool of ['Edit', 'Write', 'Read', 'NotebookEdit']) {
    const out = formatCurrentTool(tool, { file_path: '/Users/ewills/private-repo/deep/nested/file.ts' });
    assert.equal(out, `${tool}|file.ts`);
    assert.ok(!out.includes('/'));
    assert.ok(!out.includes('private-repo'));
  }
});

test('other tools: name only, no target (covers arbitrary MCP tool args)', () => {
  const out = formatCurrentTool('mcp__gmail__send_message', { to: 'someone@example.com', body: 'secret' });
  assert.equal(out, 'mcp__gmail__send_message'.slice(0, 24));
  assert.ok(!out.includes('someone'));
  assert.ok(!out.includes('secret'));
});

test('non-ASCII is stripped to ?', () => {
  assert.equal(stripNonAscii('héllo\u0007world'), 'h?llo?world');
});

test('missing tool_input does not throw and returns name only', () => {
  assert.equal(formatCurrentTool('Bash', undefined), 'Bash');
  assert.equal(formatCurrentTool('Edit', {}), 'Edit');
});

test('projectLabel prefers repo name, falls back to basename of project dir, never a full path', () => {
  assert.equal(projectLabel('my-repo', '/Users/ewills/work/my-repo'), 'my-repo');
  assert.equal(projectLabel(null, '/Users/ewills/work/some-dir'), 'some-dir');
  assert.equal(projectLabel(undefined, undefined), '');
});

test('current_tool never exceeds the protocol char[24] budget even for long names', () => {
  const out = formatCurrentTool('mcp__a-very-long-server-name__a-very-long-tool-name', {});
  assert.ok(out.length <= 24);
});
