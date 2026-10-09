import {
  StateMsg,
  SessionsMsg,
  SessionRowMsg,
  Sentinel,
  Flags,
  PFlags,
  PROTO_VERSION,
  State,
} from './protocol.gen';
import { SessionTable } from './state';
import { RateLimitTracker } from './ratelimits';
import { Session } from './types';

export interface EncodeOpts {
  now?: number; // ms epoch
  incident?: boolean;
  quietHours?: boolean;
  seq?: number;
}

function pctOrUnknown(v: number | null): number {
  return v == null ? Sentinel.PCT_UNKNOWN : Math.max(0, Math.min(100, Math.round(v)));
}

function paceOrUnknown(v: number | null): number {
  if (v == null) return Sentinel.PACE_UNKNOWN;
  return Math.max(-128, Math.min(127, Math.round(v)));
}

function u16OrUnknown(v: number | null): number {
  return v == null ? Sentinel.U16_UNKNOWN : Math.max(0, Math.min(65534, Math.round(v)));
}

function sessionRow(s: Session, now: number): SessionRowMsg {
  return {
    state: s.phase,
    ctx: pctOrUnknown(s.contextPct),
    project: s.projectRaw,
    turnElapsedS: s.turnStartAt != null ? Math.max(0, Math.round((now - s.turnStartAt) / 1000)) : 0,
  };
}

export function buildSessions(table: SessionTable, opts: EncodeOpts = {}): SessionsMsg {
  const now = opts.now ?? Date.now();
  const rows = table.orderedForRows().map((s) => sessionRow(s, now));
  return {
    version: PROTO_VERSION,
    seq: opts.seq ?? 0,
    count: rows.length,
    rows,
  };
}

export function buildState(
  table: SessionTable,
  rl: RateLimitTracker,
  opts: EncodeOpts = {},
): StateMsg {
  const now = opts.now ?? Date.now();
  const nowS = Math.floor(now / 1000);

  const five = rl.state.fiveHour;
  const seven = rl.state.sevenDay;
  const noRateLimits = rl.noRateLimits();
  const rlStale = rl.isStale(now);

  const rateLimitedNow = (five.usedPct != null && five.usedPct >= 100) || (seven.usedPct != null && seven.usedPct >= 100);
  const globalState = table.globalState(rateLimitedNow);

  const rows = table.orderedForRows();
  const primaryId = table.primaryId();
  const primaryIndex = primaryId == null ? 255 : rows.findIndex((r) => r.id === primaryId);
  const primary = primaryId != null ? table.get(primaryId) : undefined;

  let flags = 0;
  if (rlStale) flags |= Flags.RL_STALE;
  if (opts.incident) flags |= Flags.INCIDENT;
  if (table.list().some((s) => s.quiet)) flags |= Flags.QUIET;
  flags |= Flags.BRIDGE_OK;
  if (noRateLimits) flags |= Flags.NO_RATE_LIMITS;
  if (opts.quietHours) flags |= Flags.QUIET_HOURS;

  let pFlags = 0;
  if (primary) {
    if (primary.fastMode) pFlags |= PFlags.FAST_MODE;
    if (primary.thinking) pFlags |= PFlags.THINKING;
    if (primary.cacheWarm) pFlags |= PFlags.CACHE_WARM;
    if (primary.quiet) pFlags |= PFlags.HUNG;
    if (primary.unackedDone) pFlags |= PFlags.UNACKED_DONE;
    if (primary.selected) pFlags |= PFlags.SELECTED;
  }

  const msg: StateMsg = {
    version: PROTO_VERSION,
    seq: opts.seq ?? 0,
    flags,
    globalState,
    fiveUsed: noRateLimits ? Sentinel.PCT_UNKNOWN : pctOrUnknown(five.usedPct),
    fiveResetsAt: five.resetsAt ?? 0,
    sevenUsed: noRateLimits ? Sentinel.PCT_UNKNOWN : pctOrUnknown(seven.usedPct),
    sevenResetsAt: seven.resetsAt ?? 0,
    fivePace: noRateLimits ? Sentinel.PACE_UNKNOWN : paceOrUnknown(rl.pace(five, rl.windowLen('five'), now)),
    sevenPace: noRateLimits ? Sentinel.PACE_UNKNOWN : paceOrUnknown(rl.pace(seven, rl.windowLen('seven'), now)),
    projectedHit: noRateLimits ? 0 : rl.projectedHit(five, now),
    rlAgeS: rl.ageSeconds(now) ?? 65535,
    fiveElapsed: noRateLimits ? Sentinel.PCT_UNKNOWN : pctOrUnknown(rl.elapsedPct(five, rl.windowLen('five'), now)),
    sevenElapsed: noRateLimits ? Sentinel.PCT_UNKNOWN : pctOrUnknown(rl.elapsedPct(seven, rl.windowLen('seven'), now)),
    sessionCount: table.list().length,
    blockedCount: table.blockedCount(),
    subagentCount: primary?.subagentCount ?? 0,
    primaryIndex: primaryIndex < 0 ? 255 : primaryIndex,
    pState: primary?.phase ?? State.NONE,
    pCtx: pctOrUnknown(primary?.contextPct ?? null),
    pModel: primary?.model ?? 0,
    pEffort: primary?.effort ?? 0,
    pFlags,
    pTurnElapsedS: primary?.turnStartAt != null ? Math.max(0, Math.round((now - primary.turnStartAt) / 1000)) : 0,
    pLastTurnS: primary?.lastTurnDurationS ?? 0,
    pToolCount: primary?.toolCount ?? 0,
    pCurrentTool: primary?.currentTool ?? '',
    pProject: primary?.projectRaw ?? '',
    pModelName: primary?.modelDisplayName ?? '',
    pCacheExpiresAt: primary?.cacheExpiresAt ?? 0,
    pLinesAdded: primary?.linesAdded ?? 0,
    pLinesRemoved: primary?.linesRemoved ?? 0,
    pCostCents: u16OrUnknown(primary?.costCents ?? null),
    costTodayCents: u16OrUnknown(table.costTodayCents(now)),
    pPrNumber: primary?.prNumber ?? 0,
    pPrReview: primary?.prReview ?? 0,
    bridgeNow: nowS,
  };
  return msg;
}
