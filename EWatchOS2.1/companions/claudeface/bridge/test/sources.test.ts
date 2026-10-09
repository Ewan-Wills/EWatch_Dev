import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { modelDisplayName, parseTranscriptTail } from '../src/transcript';
import { parseUsage } from '../src/oauthUsage';

test('model ids map to display names', () => {
  assert.equal(modelDisplayName('claude-opus-5-5'), 'Opus 5.5');
  assert.equal(modelDisplayName('claude-sonnet-5'), 'Sonnet 5');
  assert.equal(modelDisplayName('claude-haiku-4-5-20251001'), 'Haiku 4.5');
  assert.equal(modelDisplayName('claude-fable-5-1'), 'Fable 5.1');
});

test('transcript tail: last main-thread assistant usage, cut first line tolerated', () => {
  const lines = [
    'ated garbage from a cut record"}',
    JSON.stringify({ type: 'assistant', message: { model: 'claude-sonnet-5', usage: { input_tokens: 10, cache_read_input_tokens: 50000 } } }),
    JSON.stringify({ type: 'user', message: { content: 'SECRET' } }),
    JSON.stringify({ type: 'assistant', isSidechain: true, message: { model: 'claude-haiku-4-5', usage: { input_tokens: 999999 } } }),
    '',
  ].join('\n');
  const i = parseTranscriptTail(lines)!;
  assert.equal(i.modelDisplayName, 'Sonnet 5');
  assert.equal(i.contextTokens, 50010);
  assert.equal(i.contextPct, 25); // of 200k
});

test('transcript: > 200k tokens implies the 1M window', () => {
  const t = JSON.stringify({ type: 'assistant', message: { model: 'claude-opus-5-5', usage: { input_tokens: 2, cache_creation_input_tokens: 591, cache_read_input_tokens: 370918 } } });
  assert.equal(parseTranscriptTail(t)!.contextPct, 37);
});

test('usage endpoint body -> windows (percent + epoch s), missing windows absent', () => {
  const u = parseUsage({ five_hour: { utilization: 16.0, resets_at: '2026-09-23T15:00:00.228626+00:00' }, seven_day: null });
  assert.equal(u.fiveHour!.usedPercentage, 16);
  assert.equal(u.fiveHour!.resetsAt, Math.floor(Date.parse('2026-09-23T15:00:00Z') / 1000));
  assert.equal(u.sevenDay, undefined);
});

import { transcriptEnd } from '../src/transcript';
import { SessionTable } from '../src/state';
import { State } from '../src/protocol.gen';

test('transcriptEnd: trailing cost-state = exited; interrupt marker = interrupted', () => {
  const rec = (o: object) => JSON.stringify(o);
  const a = rec({ type: 'assistant', message: { content: [{ type: 'text', text: 'hi' }] } });
  assert.equal(transcriptEnd([a, rec({ type: 'last-prompt' }), rec({ type: 'cost-state' })].join('\n')), 'exited');
  assert.equal(transcriptEnd([rec({ type: 'cost-state' }), a, rec({ type: 'last-prompt' })].join('\n')), 'other');
  const intr = rec({ type: 'user', message: { content: [{ type: 'text', text: '[Request interrupted by user]' }] } });
  assert.equal(transcriptEnd([a, intr, rec({ type: 'last-prompt' })].join('\n')), 'interrupted');
  assert.equal(transcriptEnd([a, rec({ type: 'user', message: { content: 'do the thing' } })].join('\n')), 'other');
});

test('a quiet (hung) WORKING session no longer drives the global state', () => {
  const t = new SessionTable();
  let now = 1_000_000;
  t.now = () => now;
  t.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 'dead' });
  t.handleHook({ hook_event_name: 'SessionStart', session_id: 'live' });
  assert.equal(t.globalState(), State.WORKING);
  now += 11 * 60 * 1000;
  t.handleHook({ hook_event_name: 'Notification', session_id: 'live', notification_type: 'auth_success' });
  t.reap(now);
  assert.equal(t.get('dead')!.quiet, true);
  assert.notEqual(t.globalState(), State.WORKING);
  assert.equal(t.primaryId(), 'live');
});

test('interrupt marker ends the turn', () => {
  const t = new SessionTable();
  t.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's' });
  assert.equal(t.markInterrupted('s'), true);
  assert.equal(t.get('s')!.phase, State.IDLE);
  assert.equal(t.globalState(), State.IDLE);
});
