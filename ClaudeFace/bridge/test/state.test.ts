import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { SessionTable } from '../src/state';
import { State } from '../src/protocol.gen';
import { REAP_STALE_MS, REAP_REMOVE_MS } from '../src/types';

function withClock(t0: number) {
  let now = t0;
  const table = new SessionTable();
  table.now = () => now;
  return {
    table,
    advance(ms: number) {
      now += ms;
    },
    set(ms: number) {
      now = ms;
    },
  };
}

test('SessionStart creates a session in IDLE', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'SessionStart', session_id: 's1' });
  assert.equal(table.get('s1')?.phase, State.IDLE);
});

test('UserPromptSubmit -> WORKING, starts turn timer, clears unacked DONE', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Stop', session_id: 's1' });
  assert.equal(table.get('s1')!.unackedDone, true);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  const s = table.get('s1')!;
  assert.equal(s.phase, State.WORKING);
  assert.equal(s.unackedDone, false);
  assert.notEqual(s.turnStartAt, null);
});

test('PreToolUse sets WORKING + formatted current_tool (Bash truncated to 24 chars first line)', () => {
  const { table } = withClock(1000);
  table.handleHook({
    hook_event_name: 'PreToolUse',
    session_id: 's1',
    tool_name: 'Bash',
    tool_input: { command: 'npm run build -- --verbose --extra-flag-that-is-long\nsecond line' },
  });
  const s = table.get('s1')!;
  assert.equal(s.phase, State.WORKING);
  assert.ok(s.currentTool.startsWith('Bash|'));
  assert.ok(s.currentTool.length <= 24);
  assert.ok(!s.currentTool.includes('second line'));
});

test('PreToolUse on Edit/Write/Read uses basename only, never an absolute path', () => {
  const { table } = withClock(1000);
  table.handleHook({
    hook_event_name: 'PreToolUse',
    session_id: 's1',
    tool_name: 'Edit',
    tool_input: { file_path: '/Users/ewills/secret-project/src/index.ts' },
  });
  const tool = table.get('s1')!.currentTool;
  assert.equal(tool, 'Edit|index.ts');
  assert.ok(!tool.includes('/'));
  assert.ok(!tool.includes('secret-project'));
});

test('PostToolUse and PostToolUseFailure both increment tool count and stay WORKING', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  table.handleHook({ hook_event_name: 'PostToolUse', session_id: 's1' });
  table.handleHook({ hook_event_name: 'PostToolUseFailure', session_id: 's1' });
  const s = table.get('s1')!;
  assert.equal(s.toolCount, 2);
  assert.equal(s.phase, State.WORKING);
});

test('PermissionRequest -> BLOCKED; PermissionDenied returns to WORKING mid-turn', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  table.handleHook({ hook_event_name: 'PermissionRequest', session_id: 's1' });
  assert.equal(table.get('s1')!.phase, State.BLOCKED);
  table.handleHook({ hook_event_name: 'PermissionDenied', session_id: 's1' });
  assert.equal(table.get('s1')!.phase, State.WORKING);
});

test('Notification permission_prompt -> BLOCKED (backup path)', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Notification', session_id: 's1', notification_type: 'permission_prompt' });
  assert.equal(table.get('s1')!.phase, State.BLOCKED);
});

test('Notification idle_prompt -> WAITING_INPUT', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Notification', session_id: 's1', notification_type: 'idle_prompt' });
  assert.equal(table.get('s1')!.phase, State.WAITING_INPUT);
});

test('Notification elicitation_dialog -> BLOCKED, agent_needs_input -> WAITING_INPUT', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Notification', session_id: 's1', notification_type: 'elicitation_dialog' });
  assert.equal(table.get('s1')!.phase, State.BLOCKED);
  const { table: t2 } = withClock(1000);
  t2.handleHook({ hook_event_name: 'Notification', session_id: 's2', notification_type: 'agent_needs_input' });
  assert.equal(t2.get('s2')!.phase, State.WAITING_INPUT);
});

test('Notification unrelated types (auth_success etc.) do not change phase', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  table.handleHook({ hook_event_name: 'Notification', session_id: 's1', notification_type: 'auth_success' });
  assert.equal(table.get('s1')!.phase, State.WORKING);
});

test('Stop -> DONE, records turn duration, unacked', () => {
  const { table, advance } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  advance(5000);
  table.handleHook({ hook_event_name: 'Stop', session_id: 's1' });
  const s = table.get('s1')!;
  assert.equal(s.phase, State.DONE);
  assert.equal(s.unackedDone, true);
  assert.equal(s.lastTurnDurationS, 5);
  assert.equal(s.turnStartAt, null);
});

test('SubagentStart/SubagentStop track count, never below zero', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'SubagentStop', session_id: 's1' });
  assert.equal(table.get('s1')!.subagentCount, 0);
  table.handleHook({ hook_event_name: 'SubagentStart', session_id: 's1' });
  table.handleHook({ hook_event_name: 'SubagentStart', session_id: 's1' });
  table.handleHook({ hook_event_name: 'SubagentStop', session_id: 's1' });
  assert.equal(table.get('s1')!.subagentCount, 1);
});

test('PreCompact -> COMPACTING, PostCompact restores prior phase', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  table.handleHook({ hook_event_name: 'PreCompact', session_id: 's1' });
  assert.equal(table.get('s1')!.phase, State.COMPACTING);
  table.handleHook({ hook_event_name: 'PostCompact', session_id: 's1' });
  assert.equal(table.get('s1')!.phase, State.WORKING);
});

test('SessionEnd removes the session', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'SessionStart', session_id: 's1' });
  table.handleHook({ hook_event_name: 'SessionEnd', session_id: 's1' });
  assert.equal(table.get('s1'), undefined);
});

test('StopFailure rate_limit -> LIMITED; other errors -> DONE', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  table.handleHook({ hook_event_name: 'StopFailure', session_id: 's1', error: 'rate_limit' });
  assert.equal(table.get('s1')!.phase, State.LIMITED);

  const { table: t2 } = withClock(1000);
  t2.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's2' });
  t2.handleHook({ hook_event_name: 'StopFailure', session_id: 's2', error: 'invalid_request' });
  assert.equal(t2.get('s2')!.phase, State.DONE);
});

test('quota_auto_resume_fired clears LIMITED back to IDLE/WORKING', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'StopFailure', session_id: 's1', error: 'rate_limit' });
  assert.equal(table.get('s1')!.phase, State.LIMITED);
  table.handleHook({ hook_event_name: 'Notification', session_id: 's1', notification_type: 'quota_auto_resume_fired' });
  assert.equal(table.get('s1')!.phase, State.IDLE);
});

test('global state priority: BLOCKED beats everything else', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 'a' }); // WORKING
  table.handleHook({ hook_event_name: 'PermissionRequest', session_id: 'b' }); // BLOCKED
  table.handleHook({ hook_event_name: 'Stop', session_id: 'c' }); // DONE unacked
  assert.equal(table.globalState(), State.BLOCKED);
  assert.equal(table.primaryId(), 'b');
});

test('global state priority: LIMITED beats WAITING_INPUT/WORKING/COMPACTING/DONE/IDLE', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Notification', session_id: 'a', notification_type: 'idle_prompt' });
  table.handleHook({ hook_event_name: 'StopFailure', session_id: 'b', error: 'rate_limit' });
  assert.equal(table.globalState(), State.LIMITED);
});

test('global state: rate-limit >=100% forces at least LIMITED with no BLOCKED session', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 'a' });
  assert.equal(table.globalState(true), State.LIMITED);
});

test('acked DONE ranks like IDLE for priority purposes', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'Stop', session_id: 'a' });
  table.ack('a');
  table.handleHook({ hook_event_name: 'Notification', session_id: 'b', notification_type: 'idle_prompt' });
  assert.equal(table.globalState(), State.WAITING_INPUT);
});

test('SELECT_SESSION overrides primary selection until session removed', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'SessionStart', session_id: 'a' });
  table.handleHook({ hook_event_name: 'PermissionRequest', session_id: 'b' }); // would normally be primary (BLOCKED)
  table.select('a');
  assert.equal(table.primaryId(), 'a');
  table.handleHook({ hook_event_name: 'SessionEnd', session_id: 'a' });
  assert.equal(table.primaryId(), 'b');
});

test('reap: WORKING session idle >10min is flagged quiet, not demoted', () => {
  const { table, advance } = withClock(1000);
  table.handleHook({ hook_event_name: 'UserPromptSubmit', session_id: 's1' });
  advance(REAP_STALE_MS + 1000);
  table.reap();
  const s = table.get('s1')!;
  assert.equal(s.phase, State.WORKING);
  assert.equal(s.quiet, true);
});

test('reap: non-WORKING session idle >10min -> STALE, then removed after >1h', () => {
  const { table, advance } = withClock(1000);
  table.handleHook({ hook_event_name: 'SessionStart', session_id: 's1' });
  advance(REAP_STALE_MS + 1000);
  table.reap();
  assert.equal(table.get('s1')!.phase, State.STALE);
  advance(REAP_REMOVE_MS);
  table.reap();
  assert.equal(table.get('s1'), undefined);
});

test('reap is a no-op for freshly active sessions', () => {
  const { table, advance } = withClock(1000);
  table.handleHook({ hook_event_name: 'SessionStart', session_id: 's1' });
  advance(1000);
  table.reap();
  assert.equal(table.get('s1')!.phase, State.IDLE);
  assert.equal(table.get('s1')!.quiet, false);
});

test('costTodayCents sums per-session max cost for sessions active today', () => {
  const { table } = withClock(Date.parse('2026-09-23T12:00:00'));
  table.applyStatusline('s1', { costCents: 120 });
  table.applyStatusline('s1', { costCents: 340 });
  table.applyStatusline('s2', { costCents: 50 });
  assert.equal(table.costTodayCents(Date.parse('2026-09-23T13:00:00')), 390);
});

test('PermissionRequest shows the tool being asked for, not the previous one', () => {
  const { table } = withClock(1000);
  table.handleHook({ hook_event_name: 'PreToolUse', session_id: 's1', tool_name: 'Edit', tool_input: { file_path: '/a/b/main.ts' } });
  table.handleHook({ hook_event_name: 'PermissionRequest', session_id: 's1', tool_name: 'Bash', tool_input: { command: 'rm -rf build' } });
  assert.equal(table.get('s1')!.currentTool, 'Bash|rm -rf build');
});
