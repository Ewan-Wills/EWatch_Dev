/**
 * 5h / 7d plan usage from Claude's OAuth usage endpoint (UNDOCUMENTED — may
 * change without notice). Enabled by Ewan on 2026-09-23 because desktop-app
 * sessions never run the statusline, the only documented source.
 *
 * - The token is read from the macOS Keychain item Claude Code maintains
 *   ("Claude Code-credentials") on every poll, so Claude Code's own refreshes
 *   are picked up. The bridge never refreshes, stores or logs it; it goes only
 *   to api.anthropic.com.
 * - Polled no faster than every 5 min. Any failure is silent apart from one
 *   log line per distinct error; the rings then age into RL_STALE.
 */
import { execFile } from 'child_process';

const ENDPOINT = 'https://api.anthropic.com/api/oauth/usage';
export const MIN_POLL_MS = 5 * 60 * 1000;

export interface UsageWindow {
  usedPercentage: number;
  resetsAt: number | null; // epoch s
}
export interface UsageResult {
  fiveHour?: UsageWindow;
  sevenDay?: UsageWindow;
}

function readToken(): Promise<string | null> {
  return new Promise((resolve) => {
    execFile(
      'security',
      ['find-generic-password', '-s', 'Claude Code-credentials', '-w'],
      { timeout: 5000 },
      (err, stdout) => {
        if (err) return resolve(null);
        try {
          const o = JSON.parse(stdout).claudeAiOauth;
          if (!o?.accessToken) return resolve(null);
          if (typeof o.expiresAt === 'number' && o.expiresAt < Date.now()) return resolve(null); // Claude Code will refresh it
          resolve(o.accessToken as string);
        } catch {
          resolve(null);
        }
      },
    );
  });
}

function toWindow(w: unknown): UsageWindow | undefined {
  const o = w as { utilization?: unknown; resets_at?: unknown } | null;
  if (!o || typeof o.utilization !== 'number') return undefined;
  const t = typeof o.resets_at === 'string' ? Date.parse(o.resets_at) : NaN;
  return { usedPercentage: o.utilization, resetsAt: Number.isFinite(t) ? Math.floor(t / 1000) : null };
}

export function parseUsage(body: unknown): UsageResult {
  const b = body as { five_hour?: unknown; seven_day?: unknown };
  return { fiveHour: toWindow(b?.five_hour), sevenDay: toWindow(b?.seven_day) };
}

export async function fetchUsage(): Promise<UsageResult | { error: string }> {
  const token = await readToken();
  if (!token) return { error: 'no usable token in Keychain' };
  try {
    const res = await fetch(ENDPOINT, {
      headers: { Authorization: `Bearer ${token}`, 'anthropic-beta': 'oauth-2025-04-20', 'User-Agent': 'claude-bridge/0.1' },
      signal: AbortSignal.timeout(10_000),
    });
    if (!res.ok) return { error: `HTTP ${res.status}` };
    return parseUsage(await res.json());
  } catch (e) {
    return { error: (e as Error).name };
  }
}

export class OAuthUsagePoller {
  private timer: NodeJS.Timeout | null = null;
  private lastError = '';

  constructor(
    private readonly onUsage: (u: UsageResult) => void,
    private readonly intervalMs = MIN_POLL_MS,
  ) {}

  start(): void {
    const tick = async () => {
      const r = await fetchUsage();
      if ('error' in r) {
        if (r.error !== this.lastError) {
          // eslint-disable-next-line no-console
          console.error(`[claude-bridge] oauth usage: ${r.error}`);
          this.lastError = r.error;
        }
      } else {
        this.lastError = '';
        this.onUsage(r);
      }
    };
    void tick();
    this.timer = setInterval(() => void tick(), Math.max(MIN_POLL_MS, this.intervalMs));
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
  }
}
