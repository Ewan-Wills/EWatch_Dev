import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import { RateLimitTracker } from '../src/ratelimits';
import { FIVE_HOUR_S } from '../src/types';

test('NO_RATE_LIMITS: never reported -> noRateLimits() true, no crash on math', () => {
  const rl = new RateLimitTracker(null);
  assert.equal(rl.noRateLimits(), true);
  assert.equal(rl.elapsedPct(rl.state.fiveHour, FIVE_HOUR_S), null);
  assert.equal(rl.pace(rl.state.fiveHour, FIVE_HOUR_S), null);
});

test('after one report, everReported flips and elapsed/pace compute', () => {
  const rl = new RateLimitTracker(null);
  const now = Date.parse('2026-09-23T12:00:00Z');
  const resetsAt = Math.floor(now / 1000) + FIVE_HOUR_S / 2; // halfway through the window
  rl.update({ usedPercentage: 30, resetsAt }, undefined, now);
  assert.equal(rl.noRateLimits(), false);
  const elapsed = rl.elapsedPct(rl.state.fiveHour, FIVE_HOUR_S, now)!;
  assert.ok(Math.abs(elapsed - 50) < 0.01, `expected ~50, got ${elapsed}`);
  const pace = rl.pace(rl.state.fiveHour, FIVE_HOUR_S, now)!;
  assert.ok(Math.abs(pace - (30 - 50)) < 0.01);
});

test('window expires once resets_at passes', () => {
  const rl = new RateLimitTracker(null);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  rl.update({ usedPercentage: 99, resetsAt: Math.floor(t0 / 1000) + 10 }, undefined, t0);
  assert.equal(rl.state.fiveHour.usedPct, 99);
  rl.expire(t0 + 11_000);
  assert.equal(rl.state.fiveHour.usedPct, null);
  assert.equal(rl.state.fiveHour.resetsAt, null);
});

test('isStale flips true after 15 min without an update', () => {
  const rl = new RateLimitTracker(null);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  rl.update({ usedPercentage: 10, resetsAt: Math.floor(t0 / 1000) + FIVE_HOUR_S }, undefined, t0);
  assert.equal(rl.isStale(t0 + 5 * 60 * 1000), false);
  assert.equal(rl.isStale(t0 + 16 * 60 * 1000), true);
});

test('isStale is false (not true) when there is simply no data yet', () => {
  const rl = new RateLimitTracker(null);
  assert.equal(rl.isStale(Date.now()), false);
});

test('projectedHit: rising usage projects a hit time before reset', () => {
  const rl = new RateLimitTracker(null);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  const resetsAt = Math.floor(t0 / 1000) + 3 * 3600; // 3h away
  // Simulate samples climbing steeply: 10% -> 90% over 20 minutes.
  rl.update({ usedPercentage: 10, resetsAt }, undefined, t0);
  rl.update({ usedPercentage: 90, resetsAt }, undefined, t0 + 20 * 60 * 1000);
  const hit = rl.projectedHit(rl.state.fiveHour, t0 + 20 * 60 * 1000);
  assert.ok(hit > 0, 'expected a nonzero projected hit time');
  assert.ok(hit < resetsAt, 'projected hit should be before reset');
});

test('projectedHit: flat/falling usage -> 0 (never)', () => {
  const rl = new RateLimitTracker(null);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  const resetsAt = Math.floor(t0 / 1000) + 3 * 3600;
  rl.update({ usedPercentage: 50, resetsAt }, undefined, t0);
  rl.update({ usedPercentage: 50, resetsAt }, undefined, t0 + 20 * 60 * 1000);
  const hit = rl.projectedHit(rl.state.fiveHour, t0 + 20 * 60 * 1000);
  assert.equal(hit, 0);
});

test('projectedHit: would hit after reset -> 0', () => {
  const rl = new RateLimitTracker(null);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  const resetsAt = Math.floor(t0 / 1000) + 60; // resets in 1 minute
  rl.update({ usedPercentage: 10, resetsAt }, undefined, t0);
  rl.update({ usedPercentage: 11, resetsAt }, undefined, t0 + 20 * 60 * 1000);
  const hit = rl.projectedHit(rl.state.fiveHour, t0 + 20 * 60 * 1000);
  assert.equal(hit, 0);
});

test('persistence round-trip via loadFromDisk', () => {
  const os = require('os');
  const path = require('path');
  const fs = require('fs');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cb-rl-'));
  const file = path.join(dir, 'ratelimits.json');
  const rl = new RateLimitTracker(file);
  const t0 = Date.parse('2026-09-23T12:00:00Z');
  rl.update({ usedPercentage: 42, resetsAt: Math.floor(t0 / 1000) + FIVE_HOUR_S }, undefined, t0);
  const rl2 = RateLimitTracker.loadFromDisk(file);
  assert.equal(rl2.state.fiveHour.usedPct, 42);
  assert.equal(rl2.noRateLimits(), false);
  fs.rmSync(dir, { recursive: true, force: true });
});

import { parseStatusline } from '../src/statusline';
test('statusline carries lines added/removed through', () => {
  const p = parseStatusline({ session_id: 's', cost: { total_cost_usd: 1, total_lines_added: 120, total_lines_removed: 7 } } as never);
  assert.equal(p.session.linesAdded, 120);
  assert.equal(p.session.linesRemoved, 7);
});
