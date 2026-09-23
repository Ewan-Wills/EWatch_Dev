import { Bridge } from './bridge';

const STATUS_URL = 'https://status.anthropic.com/api/v2/status.json';
const POLL_MS = 5 * 60 * 1000;

/** Optional, off by default (SPEC 2.6a). Polls the Anthropic Statuspage JSON and
 *  sets the INCIDENT flag when indicator != "none". Never enabled unless
 *  config.statuspage.enabled is true. */
export class StatuspagePoller {
  private timer: NodeJS.Timeout | null = null;

  constructor(private bridge: Bridge) {}

  start(): void {
    this.pollOnce();
    this.timer = setInterval(() => this.pollOnce(), POLL_MS);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
  }

  private async pollOnce(): Promise<void> {
    try {
      const controller = new AbortController();
      const t = setTimeout(() => controller.abort(), 5000);
      const res = await fetch(STATUS_URL, { signal: controller.signal });
      clearTimeout(t);
      if (!res.ok) return;
      const json = (await res.json()) as { status?: { indicator?: string } };
      this.bridge.setIncident((json.status?.indicator ?? 'none') !== 'none');
    } catch {
      // never crash the bridge over a status-page hiccup; leave last-known flag as-is
    }
  }
}
