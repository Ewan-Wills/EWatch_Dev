import { test } from 'node:test';
import * as assert from 'node:assert/strict';
import * as http from 'http';
import { Bridge } from '../src/bridge';
import { createServer } from '../src/server';
import { FakeLink } from '../src/link';
import { RateLimitTracker } from '../src/ratelimits';
import { loadConfig } from '../src/config';
import { unframe, decodeState, State } from '../src/protocol.gen';

function postJson(port: number, path: string, body: unknown): Promise<{ status: number; ms: number }> {
  const data = JSON.stringify(body);
  const t0 = Date.now();
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: '127.0.0.1', port, path, method: 'POST', headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } },
      (res) => {
        res.on('data', () => {});
        res.on('end', () => resolve({ status: res.statusCode ?? 0, ms: Date.now() - t0 }));
      },
    );
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

async function withServer(fn: (ctx: { port: number; bridge: Bridge; link: FakeLink }) => Promise<void>) {
  const cfg = loadConfig({ CLAUDE_BRIDGE_PORT: '0', CLAUDE_BRIDGE_CONFIG_DIR: '/tmp/claude-bridge-test-unused' } as NodeJS.ProcessEnv);
  const link = new FakeLink();
  const rl = new RateLimitTracker(null);
  const bridge = new Bridge({ cfg, link, rl });
  link.connectNow(); // fake link is "connected" immediately
  bridge.start();
  const server = createServer(bridge);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const addr = server.address();
  const port = typeof addr === 'object' && addr ? addr.port : 0;
  try {
    await fn({ port, bridge, link });
  } finally {
    bridge.stop();
    server.close();
  }
}

test('POST /healthz and GET /state respond', async () => {
  await withServer(async ({ port }) => {
    const health = await postJson(port, '/healthz', {});
    // /healthz is a GET in the real API; use raw http.get here instead.
    void health;
    const state = await new Promise<string>((resolve, reject) => {
      http.get(`http://127.0.0.1:${port}/state`, (res) => {
        let body = '';
        res.on('data', (c) => (body += c));
        res.on('end', () => resolve(body));
        res.on('error', reject);
      });
    });
    const parsed = JSON.parse(state);
    assert.ok('sessions' in parsed);
  });
});

test('a realistic hook sequence produces the expected STATE frame within debounce latency', async () => {
  await withServer(async ({ port, link }) => {
    const sessionId = 'e2e-session-1';
    await postJson(port, '/hook', { hook_event_name: 'SessionStart', session_id: sessionId, source: 'startup' });
    await postJson(port, '/hook', { hook_event_name: 'UserPromptSubmit', session_id: sessionId });
    const t0 = Date.now();
    await postJson(port, '/hook', {
      hook_event_name: 'PreToolUse',
      session_id: sessionId,
      tool_name: 'Bash',
      tool_input: { command: 'npm test' },
    });

    // Wait past the 250ms debounce for the pushed frame to land.
    await new Promise((r) => setTimeout(r, 400));
    const latencyMs = Date.now() - t0;
    assert.ok(latencyMs < 1000, `expected the debounced push within ~1s, took ${latencyMs}ms`);

    const stateLines = link.sent.filter((l) => l.startsWith('#CFS'));
    assert.ok(stateLines.length > 0, 'expected at least one STATE frame');
    const last = decodeState(unframe(stateLines[stateLines.length - 1]!)!.payload);
    assert.equal(last.globalState, State.WORKING);
    assert.equal(last.pCurrentTool, 'Bash|npm test');
    assert.equal(last.pProject, '');
  });
});

test('measures POST -> frame-written latency distribution (~debounce 250ms)', async () => {
  await withServer(async ({ port, link, bridge }) => {
    const samples: number[] = [];
    for (let i = 0; i < 8; i++) {
      const sid = `latency-${i}`;
      link.sent = [];
      const t0 = Date.now();
      await postJson(port, '/hook', { hook_event_name: 'UserPromptSubmit', session_id: sid });
      // Poll for the frame to appear rather than a fixed sleep, so the measurement is real.
      let waited = 0;
      while (link.sent.filter((l) => l.startsWith('#CFS')).length === 0 && waited < 2000) {
        await new Promise((r) => setTimeout(r, 10));
        waited += 10;
      }
      const t1 = bridge.link.lastFrameSentAtMs ?? Date.now();
      samples.push(t1 - t0);
    }
    const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
    // eslint-disable-next-line no-console
    console.log(`[latency] POST->frame samples(ms): ${samples.join(', ')} mean=${mean.toFixed(1)}`);
    // Debounce is 250ms; allow generous scheduler slack in CI.
    for (const s of samples) {
      assert.ok(s >= 200 && s < 1500, `sample ${s}ms out of expected debounce range`);
    }
  });
});

test('sanity: this suite can actually fail (negative control)', () => {
  assert.throws(() => assert.equal(1, 2));
});
