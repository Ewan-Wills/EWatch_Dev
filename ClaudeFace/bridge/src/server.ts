import * as http from 'http';
import { URL } from 'url';
import { Bridge } from './bridge';

const MAX_BODY_BYTES = 1_000_000;

function readBody(req: http.IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks: Buffer[] = [];
    req.on('data', (c: Buffer) => {
      size += c.length;
      if (size > MAX_BODY_BYTES) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function send(res: http.ServerResponse, code: number, body: unknown): void {
  const json = JSON.stringify(body);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(json) });
  res.end(json);
}

/** Only the event name + a session id PREFIX are ever logged — never event bodies. */
function logEvent(kind: string, sessionId?: string | null): void {
  const prefix = sessionId ? sessionId.slice(0, 8) : '?';
  // eslint-disable-next-line no-console
  console.log(`[claude-bridge] ${kind} session=${prefix}`);
}

export function createServer(bridge: Bridge): http.Server {
  return http.createServer((req, res) => {
    const url = new URL(req.url ?? '/', 'http://127.0.0.1');
    (async () => {
      if (req.method === 'GET' && url.pathname === '/healthz') {
        send(res, 200, { ok: true });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/state') {
        send(res, 200, {
          ...bridge.debugState(),
          last_hook_at_ms: bridge.lastHookAtMs,
          last_frame_sent_at_ms: bridge.link.lastFrameSentAtMs,
        });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/hook') {
        const body = await readBody(req);
        let parsed: unknown;
        try {
          parsed = JSON.parse(body);
        } catch {
          send(res, 400, { error: 'invalid json' });
          return;
        }
        const ev = parsed as { hook_event_name?: string; session_id?: string };
        logEvent(`hook ${ev.hook_event_name ?? '?'}`, ev.session_id);
        bridge.onHook(parsed as Parameters<Bridge['onHook']>[0]);
        send(res, 200, { ok: true });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/statusline') {
        const body = await readBody(req);
        let parsed: unknown;
        try {
          parsed = JSON.parse(body);
        } catch {
          send(res, 400, { error: 'invalid json' });
          return;
        }
        const ev = parsed as { session_id?: string };
        logEvent('statusline', ev.session_id);
        bridge.onStatusline(parsed as Parameters<Bridge['onStatusline']>[0]);
        send(res, 200, { ok: true });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/serial/release') {
        const seconds = Number(url.searchParams.get('seconds') ?? '30');
        await bridge.link.release(Number.isFinite(seconds) && seconds > 0 ? seconds : 30);
        send(res, 200, { ok: true, released_for_s: seconds });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/serial/reacquire') {
        await bridge.link.reacquire();
        send(res, 200, { ok: true });
        return;
      }
      send(res, 404, { error: 'not found' });
    })().catch((err) => {
      send(res, 500, { error: String(err?.message ?? err) });
    });
  });
}
