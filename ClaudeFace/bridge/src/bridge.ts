import {
  frame,
  encodeState,
  encodeSessions,
  encodeTime,
  PROTO_VERSION,
  EventCode,
  EventMsg,
} from './protocol.gen';
import { SessionTable, HookEvent } from './state';
import { RateLimitTracker } from './ratelimits';
import { buildState, buildSessions } from './encoder';
import { BaseLink } from './link';
import { BridgeConfig } from './config';
import { parseStatusline, StatuslinePayload, mapModel } from './statusline';
import { readTranscriptInfo, readTranscriptEnd, projectRoot } from './transcript';
import { UsageResult } from './oauthUsage';
import { isQuietHours, utcOffsetMinutes } from './util';

const DEBOUNCE_MS = 250;
const HEARTBEAT_MS = 30_000;
const TIME_RESYNC_MS = 60 * 60 * 1000;
const REAP_INTERVAL_MS = 10_000;
// Sessions silent this long get their transcript checked for an interrupt / exit.
const TRANSCRIPT_CHECK_AFTER_MS = 15_000;

export interface BridgeDeps {
  cfg: BridgeConfig;
  link: BaseLink;
  rl: RateLimitTracker;
  trace?: boolean;
  clock?: () => number; // ms epoch, injectable for tests
}

export class Bridge {
  readonly table = new SessionTable();
  readonly rl: RateLimitTracker;
  readonly link: BaseLink;
  private cfg: BridgeConfig;
  private trace: boolean;
  private clock: () => number;

  private stateSeq = 0;
  private sessionsSeq = 0;
  private pushTimer: NodeJS.Timeout | null = null;
  private heartbeatTimer: NodeJS.Timeout | null = null;
  private timeTimer: NodeJS.Timeout | null = null;
  private reapTimer: NodeJS.Timeout | null = null;
  private incidentFlag = false;

  lastHookAtMs: number | null = null;

  constructor(deps: BridgeDeps) {
    this.cfg = deps.cfg;
    this.link = deps.link;
    this.rl = deps.rl;
    this.trace = Boolean(deps.trace);
    this.clock = deps.clock ?? (() => Date.now());
    this.table.now = this.clock;

    this.link.on('connect', () => this.onConnect());
    this.link.on('event', (ev: EventMsg) => this.onWatchEvent(ev));
  }

  setIncident(v: boolean): void {
    this.incidentFlag = v;
  }

  start(): void {
    this.heartbeatTimer = setInterval(() => this.pushNow(), HEARTBEAT_MS);
    this.timeTimer = setInterval(() => this.sendTime(), TIME_RESYNC_MS);
    this.reapTimer = setInterval(() => {
      this.table.reap(this.clock());
      void this.checkTranscripts().then(() => this.schedulePush());
    }, REAP_INTERVAL_MS);
  }

  stop(): void {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    if (this.timeTimer) clearInterval(this.timeTimer);
    if (this.reapTimer) clearInterval(this.reapTimer);
    if (this.pushTimer) clearTimeout(this.pushTimer);
  }

  private onConnect(): void {
    this.sendTime();
    this.pushNow();
  }

  private onWatchEvent(ev: EventMsg): void {
    if (ev.code === EventCode.ACK) {
      const primary = this.table.primaryId();
      if (primary) this.table.ack(primary);
      this.schedulePush();
    } else if (ev.code === EventCode.SELECT_SESSION) {
      const rows = this.table.orderedForRows();
      const row = rows[ev.arg];
      if (row) this.table.select(row.id);
      this.schedulePush();
    }
  }

  onHook(ev: HookEvent): void {
    this.lastHookAtMs = this.clock();
    // Project = git root of the session's directory, so a `cd` inside the
    // session doesn't rename it.
    if (typeof ev.cwd === 'string' && ev.cwd) ev.cwd = projectRoot(ev.cwd);
    this.table.handleHook(ev);
    this.schedulePush();
    void this.enrichFromTranscript(ev);
  }

  /** Model + context % for sessions that never run the statusline (desktop app). */
  private async enrichFromTranscript(ev: HookEvent): Promise<void> {
    if (typeof ev.transcript_path !== 'string') return;
    const info = await readTranscriptInfo(ev.transcript_path, this.clock());
    if (!info || !this.table.get(ev.session_id)) return; // throttled, unreadable, or session ended
    this.table.applyStatusline(ev.session_id, {
      modelDisplayName: info.modelDisplayName,
      model: mapModel(info.modelDisplayName),
      contextPct: info.contextPct,
    });
    this.schedulePush();
  }

  /** Catch turns that ended by interrupt and sessions that exited without a hook. */
  private async checkTranscripts(): Promise<void> {
    const now = this.clock();
    for (const s of this.table.list()) {
      if (!s.transcriptPath || now - s.lastEventAt < TRANSCRIPT_CHECK_AFTER_MS) continue;
      const r = await readTranscriptEnd(s.transcriptPath);
      if (!r) continue;
      // A resumed session fires SessionStart before writing anything, so an
      // old trailing cost-state only counts if it was written after our last
      // hook from that session.
      if (r.end === 'exited' && r.mtimeMs >= s.lastEventAt - 2000) this.table.remove(s.id);
      else if (r.end === 'interrupted') this.table.markInterrupted(s.id);
    }
  }

  onUsage(u: UsageResult): void {
    this.rl.update(u.fiveHour, u.sevenDay, this.clock());
    this.schedulePush();
  }

  onStatusline(payload: StatuslinePayload): void {
    const parsed = parseStatusline(payload);
    if (parsed.sessionId) {
      this.table.applyStatusline(parsed.sessionId, parsed.session);
    }
    this.rl.update(parsed.rateLimits.fiveHour, parsed.rateLimits.sevenDay, this.clock());
    this.schedulePush();
  }

  private schedulePush(): void {
    if (this.pushTimer) clearTimeout(this.pushTimer);
    this.pushTimer = setTimeout(() => this.pushNow(), DEBOUNCE_MS);
  }

  pushNow(): void {
    const now = this.clock();
    const sessionsMsg = buildSessions(this.table, { now, seq: ++this.sessionsSeq });
    this.link.send(frame('L', encodeSessions(sessionsMsg)));

    const stateMsg = buildState(this.table, this.rl, {
      now,
      seq: ++this.stateSeq,
      incident: this.incidentFlag,
      quietHours: isQuietHours(this.cfg.quietHours.startMin, this.cfg.quietHours.endMin, new Date(now)),
    });
    this.link.send(frame('S', encodeState(stateMsg)));

    if (this.trace && this.lastHookAtMs != null) {
      const delta = this.link.lastFrameSentAtMs != null ? this.link.lastFrameSentAtMs - this.lastHookAtMs : null;
      if (delta != null) {
        // eslint-disable-next-line no-console
        console.error(`[trace] hook->frame ${delta}ms`);
      }
    }
  }

  sendTime(): void {
    const now = this.clock();
    const msg = { version: PROTO_VERSION, epoch: Math.floor(now / 1000), utcOffsetMin: utcOffsetMinutes(new Date(now)) };
    this.link.send(frame('T', encodeTime(msg)));
  }

  debugState(): Record<string, unknown> {
    return {
      sessions: this.table.list(),
      primary: this.table.primaryId(),
      globalState: this.table.globalState(),
      rateLimits: this.rl.state,
      lastHookAtMs: this.lastHookAtMs,
      lastFrameSentAtMs: this.link.lastFrameSentAtMs,
    };
  }
}
