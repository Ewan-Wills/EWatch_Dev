import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import {
  frame,
  unframe,
  encodeState,
  decodeState,
  encodeSessions,
  decodeSessions,
  zeroState,
  zeroSessionRow,
  PROTO_VERSION,
  State,
  Sentinel,
} from '../src/protocol.gen';

test('State round-trips through frame()/unframe()/decodeState()', () => {
  const msg = zeroState();
  msg.version = PROTO_VERSION;
  msg.seq = 42;
  msg.globalState = State.WORKING;
  msg.pCurrentTool = 'Bash|npm test';
  msg.pProject = 'ewatch';
  msg.fiveUsed = Sentinel.PCT_UNKNOWN;

  const line = frame('S', encodeState(msg));
  assert.ok(line.startsWith('#CFS '));
  assert.ok(line.endsWith('\n'));

  const decoded = unframe(line);
  assert.ok(decoded);
  assert.equal(decoded!.type, 'S');
  const roundTripped = decodeState(decoded!.payload);
  assert.equal(roundTripped.seq, 42);
  assert.equal(roundTripped.globalState, State.WORKING);
  assert.equal(roundTripped.pCurrentTool, 'Bash|npm test');
  assert.equal(roundTripped.pProject, 'ewatch');
  assert.equal(roundTripped.fiveUsed, Sentinel.PCT_UNKNOWN);
});

test('Sessions round-trips with 4 rows', () => {
  const rows = [0, 1, 2, 3].map((i) => ({ ...zeroSessionRow(), state: State.IDLE, project: `p${i}`, ctx: i * 10 }));
  const msg = { version: PROTO_VERSION, seq: 7, count: 4, rows };
  const line = frame('L', encodeSessions(msg));
  const decoded = unframe(line)!;
  const rt = decodeSessions(decoded.payload);
  assert.equal(rt.count, 4);
  assert.equal(rt.rows[2]!.project, 'p2');
  assert.equal(rt.rows[2]!.ctx, 20);
});

test('unframe rejects a corrupted line (bad CRC)', () => {
  const msg = zeroState();
  const line = frame('S', encodeState(msg));
  // Flip one base64 character in the payload to corrupt the CRC.
  const spaceIdx = line.indexOf(' ');
  const b64 = line.slice(spaceIdx + 1, -1);
  const corruptedChar = b64[5] === 'A' ? 'B' : 'A';
  const corrupted = `${line.slice(0, spaceIdx + 1)}${b64.slice(0, 5)}${corruptedChar}${b64.slice(6)}\n`;
  assert.equal(unframe(corrupted), null);
});

test('unframe rejects garbage / non-CF lines', () => {
  assert.equal(unframe('not a frame at all'), null);
  assert.equal(unframe('#CFZ !!!not-base64!!!'), null);
  assert.equal(unframe(''), null);
});

test('version-mismatch frame still decodes; caller is responsible for comparing to PROTO_VERSION', () => {
  const msg = zeroState();
  msg.version = 99;
  const line = frame('S', encodeState(msg));
  const decoded = unframe(line)!;
  const rt = decodeState(decoded.payload);
  assert.equal(rt.version, 99);
  assert.notEqual(rt.version, PROTO_VERSION);
});

test('NO_RATE_LIMITS encodes as 255 (PCT_UNKNOWN), never 0', () => {
  const msg = zeroState();
  msg.fiveUsed = Sentinel.PCT_UNKNOWN;
  msg.sevenUsed = Sentinel.PCT_UNKNOWN;
  const line = frame('S', encodeState(msg));
  const rt = decodeState(unframe(line)!.payload);
  assert.equal(rt.fiveUsed, 255);
  assert.equal(rt.sevenUsed, 255);
  assert.notEqual(rt.fiveUsed, 0);
});
