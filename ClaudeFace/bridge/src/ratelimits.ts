import * as fs from 'fs';
import { FIVE_HOUR_S, SEVEN_DAY_S, RL_STALE_MS, RateLimitsState, RateWindowState } from './types';

const SAMPLE_WINDOW_MS = 30 * 60 * 1000;

function emptyWindow(): RateWindowState {
  return { usedPct: null, resetsAt: null, lastUpdated: null, samples: [] };
}

export function emptyRateLimits(): RateLimitsState {
  return { fiveHour: emptyWindow(), sevenDay: emptyWindow(), everReported: false };
}

export interface WindowInput {
  usedPercentage?: number | null;
  resetsAt?: number | null; // epoch s
}

export class RateLimitTracker {
  state: RateLimitsState;
  private file: string | null;

  constructor(file: string | null, initial?: RateLimitsState) {
    this.file = file;
    this.state = initial ?? emptyRateLimits();
  }

  static loadFromDisk(file: string): RateLimitTracker {
    try {
      const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
      const state: RateLimitsState = {
        fiveHour: { ...emptyWindow(), ...raw.fiveHour },
        sevenDay: { ...emptyWindow(), ...raw.sevenDay },
        everReported: Boolean(raw.everReported),
      };
      return new RateLimitTracker(file, state);
    } catch {
      return new RateLimitTracker(file);
    }
  }

  persist(): void {
    if (!this.file) return;
    try {
      fs.mkdirSync(require('path').dirname(this.file), { recursive: true });
      fs.writeFileSync(this.file, JSON.stringify(this.state));
    } catch {
      // best-effort; never throw from persistence
    }
  }

  update(fiveHour: WindowInput | undefined, sevenDay: WindowInput | undefined, now = Date.now()): void {
    if (fiveHour && fiveHour.usedPercentage != null) {
      this.applyWindow(this.state.fiveHour, fiveHour, now);
      this.state.everReported = true;
    }
    if (sevenDay && sevenDay.usedPercentage != null) {
      this.applyWindow(this.state.sevenDay, sevenDay, now);
      this.state.everReported = true;
    }
    this.expire(now);
    this.persist();
  }

  private applyWindow(w: RateWindowState, input: WindowInput, now: number): void {
    w.usedPct = input.usedPercentage ?? null;
    if (input.resetsAt != null) w.resetsAt = input.resetsAt;
    w.lastUpdated = now;
    w.samples.push({ t: now, used: w.usedPct ?? 0 });
    w.samples = w.samples.filter((s) => now - s.t <= SAMPLE_WINDOW_MS);
  }

  /** Drop each window once its resets_at has passed. */
  expire(now = Date.now()): void {
    for (const w of [this.state.fiveHour, this.state.sevenDay]) {
      if (w.resetsAt != null && now >= w.resetsAt * 1000) {
        w.usedPct = null;
        w.resetsAt = null;
        w.lastUpdated = null;
        w.samples = [];
      }
    }
  }

  isStale(now = Date.now()): boolean {
    const ages = [this.state.fiveHour.lastUpdated, this.state.sevenDay.lastUpdated].filter(
      (t): t is number => t != null,
    );
    if (ages.length === 0) return false; // no data at all -> NO_RATE_LIMITS handles it, not staleness
    const freshest = Math.max(...ages);
    return now - freshest > RL_STALE_MS;
  }

  ageSeconds(now = Date.now()): number | null {
    const ages = [this.state.fiveHour.lastUpdated, this.state.sevenDay.lastUpdated].filter(
      (t): t is number => t != null,
    );
    if (ages.length === 0) return null;
    return Math.round((now - Math.max(...ages)) / 1000);
  }

  noRateLimits(): boolean {
    return !this.state.everReported;
  }

  elapsedPct(w: RateWindowState, windowLenS: number, now = Date.now()): number | null {
    if (w.resetsAt == null) return null;
    const windowStart = w.resetsAt - windowLenS;
    const elapsed = ((now / 1000 - windowStart) / windowLenS) * 100;
    return Math.max(0, Math.min(100, elapsed));
  }

  pace(w: RateWindowState, windowLenS: number, now = Date.now()): number | null {
    if (w.usedPct == null) return null;
    const elapsed = this.elapsedPct(w, windowLenS, now);
    if (elapsed == null) return null;
    return w.usedPct - elapsed;
  }

  /** Linear fit over samples from the last 30 min; epoch s when 100% would be hit if before resets_at, else 0. */
  projectedHit(w: RateWindowState, now = Date.now()): number {
    if (w.resetsAt == null || w.usedPct == null || w.samples.length < 2) return 0;
    const pts = w.samples;
    const n = pts.length;
    const tMean = pts.reduce((a, p) => a + p.t, 0) / n;
    const uMean = pts.reduce((a, p) => a + p.used, 0) / n;
    let num = 0;
    let den = 0;
    for (const p of pts) {
      num += (p.t - tMean) * (p.used - uMean);
      den += (p.t - tMean) * (p.t - tMean);
    }
    if (den === 0) return 0;
    const slopePerMs = num / den; // %/ms
    if (slopePerMs <= 0) return 0;
    const last = pts[pts.length - 1]!;
    const msToHit = (100 - last.used) / slopePerMs;
    if (msToHit < 0) return 0;
    const hitAtMs = last.t + msToHit;
    const hitAtS = Math.round(hitAtMs / 1000);
    return hitAtS < w.resetsAt ? hitAtS : 0;
  }

  windowLen(which: 'five' | 'seven'): number {
    return which === 'five' ? FIVE_HOUR_S : SEVEN_DAY_S;
  }
}
