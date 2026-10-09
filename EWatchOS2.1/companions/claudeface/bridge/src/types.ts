import { Model, Effort, PrReview } from './protocol.gen';

/** Internal session "phase" — same numbering as the protocol's State enum
 *  (NONE/IDLE/WORKING/COMPACTING/DONE/WAITING_INPUT/BLOCKED/LIMITED/STALE)
 *  so it can be written to the wire without translation. */
export type Phase = number;

export interface Session {
  id: string;
  phase: Phase;
  createdAt: number; // ms epoch
  lastEventAt: number; // ms epoch, last hook or statusline touch
  turnStartAt: number | null; // ms epoch
  lastTurnDurationS: number | null;
  toolCount: number;
  currentTool: string; // already formatted "Tool|target", ascii, <=24 chars
  subagentCount: number;
  unackedDone: boolean;
  quiet: boolean; // WORKING with no events for 10 min (possibly hung)
  transcriptPath?: string; // from hooks; used to detect interrupts / exits
  blockedSince: number | null;
  limitedReason: string | null;
  preCompactPhase: Phase | null;

  // from statusline
  projectRaw: string; // repo name or project dir basename, untruncated
  modelDisplayName: string;
  model: Model;
  effort: Effort;
  fastMode: boolean;
  thinking: boolean;
  contextPct: number | null; // 0..100, null = unknown
  costCents: number | null; // latest session cost estimate
  linesAdded: number;
  linesRemoved: number;
  cacheWarm: boolean;
  cacheExpiresAt: number | null; // epoch s
  prNumber: number | null;
  prReview: PrReview;

  selected: boolean; // watch-selected primary override
}

export interface RateWindowState {
  usedPct: number | null; // 0..100, null = unknown
  resetsAt: number | null; // epoch s
  lastUpdated: number | null; // ms epoch
  samples: Array<{ t: number; used: number }>; // last 30 min, t = ms epoch
}

export interface RateLimitsState {
  fiveHour: RateWindowState;
  sevenDay: RateWindowState;
  everReported: boolean;
}

export const FIVE_HOUR_S = 18000;
export const SEVEN_DAY_S = 604800;
export const REAP_STALE_MS = 10 * 60 * 1000;
export const REAP_REMOVE_MS = 60 * 60 * 1000;
export const RL_STALE_MS = 15 * 60 * 1000;
