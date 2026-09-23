import { State, Model, Effort, PrReview } from './protocol.gen';
import { Session, REAP_STALE_MS, REAP_REMOVE_MS } from './types';
import { formatCurrentTool, projectLabel } from './privacy';

/** Global-state priority order, highest first. */
const PRIORITY: number[] = [
  State.BLOCKED,
  State.LIMITED,
  State.WAITING_INPUT,
  State.WORKING,
  State.COMPACTING,
  State.DONE,
  State.IDLE,
  State.STALE,
  State.NONE,
];

function priorityIndex(phase: number, unackedDone: boolean): number {
  if (phase === State.DONE && !unackedDone) {
    return PRIORITY.indexOf(State.IDLE);
  }
  const idx = PRIORITY.indexOf(phase);
  return idx === -1 ? PRIORITY.length - 1 : idx;
}

export interface HookEvent {
  hook_event_name: string;
  session_id: string;
  tool_name?: string;
  tool_input?: unknown;
  notification_type?: string;
  message?: string;
  title?: string;
  error?: string;
  workspace?: { project_dir?: string; repo?: { name?: string } };
  cwd?: string;
  [k: string]: unknown;
}

const LIMITED_ERRORS = new Set(['rate_limit', 'billing_error', 'account_on_hold']);
const FILE_LIKE_NOTIFICATIONS_BLOCKED = new Set(['permission_prompt', 'elicitation_dialog', 'elicitation_url_dialog']);
const WAITING_NOTIFICATIONS = new Set(['idle_prompt', 'agent_needs_input']);

export class SessionTable {
  private sessions = new Map<string, Session>();
  private manualPrimaryId: string | null = null;
  /** sessionId -> {maxCents, day} for today's cost accounting; survives session removal same-day. */
  private costLedger = new Map<string, { maxCents: number; day: string }>();

  now(): number {
    return Date.now();
  }

  private localDay(t: number): string {
    const d = new Date(t);
    return `${d.getFullYear()}-${d.getMonth() + 1}-${d.getDate()}`;
  }

  private getOrCreate(id: string, now: number): Session {
    let s = this.sessions.get(id);
    if (!s) {
      s = {
        id,
        phase: State.IDLE,
        createdAt: now,
        lastEventAt: now,
        turnStartAt: null,
        lastTurnDurationS: null,
        toolCount: 0,
        currentTool: '',
        subagentCount: 0,
        unackedDone: false,
        quiet: false,
        blockedSince: null,
        limitedReason: null,
        preCompactPhase: null,
        projectRaw: '',
        modelDisplayName: '',
        model: Model.UNKNOWN,
        effort: Effort.UNKNOWN,
        fastMode: false,
        thinking: false,
        contextPct: null,
        costCents: null,
        linesAdded: 0,
        linesRemoved: 0,
        cacheWarm: false,
        cacheExpiresAt: null,
        prNumber: null,
        prReview: PrReview.NONE,
        selected: false,
      };
      this.sessions.set(id, s);
    }
    return s;
  }

  private touchProject(s: Session, ev: HookEvent): void {
    const repo = ev.workspace?.repo?.name;
    const dir = ev.workspace?.project_dir ?? ev.cwd;
    if (repo || dir) s.projectRaw = projectLabel(repo, dir);
  }

  handleHook(ev: HookEvent): void {
    const now = this.now();
    if (!ev || typeof ev.session_id !== 'string' || !ev.session_id) return;
    const s = this.getOrCreate(ev.session_id, now);
    if (typeof ev.transcript_path === 'string') s.transcriptPath = ev.transcript_path;
    s.lastEventAt = now;
    s.quiet = false;
    this.touchProject(s, ev);

    switch (ev.hook_event_name) {
      case 'SessionStart':
        s.phase = State.IDLE;
        break;
      case 'UserPromptSubmit':
        s.phase = State.WORKING;
        s.turnStartAt = now;
        s.toolCount = 0;
        s.currentTool = '';
        s.unackedDone = false;
        s.blockedSince = null;
        break;
      case 'PreToolUse':
        s.phase = State.WORKING;
        s.currentTool = formatCurrentTool(ev.tool_name ?? '', ev.tool_input);
        break;
      case 'PostToolUse':
      case 'PostToolUseFailure':
        s.phase = State.WORKING;
        s.toolCount += 1;
        break;
      case 'PermissionRequest':
        s.phase = State.BLOCKED;
        // Show what is being asked for, not the previous tool.
        if (ev.tool_name) s.currentTool = formatCurrentTool(ev.tool_name, ev.tool_input);
        s.blockedSince = s.blockedSince ?? now;
        break;
      case 'PermissionDenied':
        s.phase = s.turnStartAt != null ? State.WORKING : State.IDLE;
        s.blockedSince = null;
        break;
      case 'Notification':
        this.handleNotification(s, ev, now);
        break;
      case 'Stop':
        this.finishTurn(s, now);
        s.phase = State.DONE;
        s.unackedDone = true;
        s.blockedSince = null;
        break;
      case 'StopFailure': {
        const err = typeof ev.error === 'string' ? ev.error : '';
        if (LIMITED_ERRORS.has(err)) {
          s.phase = State.LIMITED;
          s.limitedReason = err;
        } else {
          this.finishTurn(s, now);
          s.phase = State.DONE;
          s.unackedDone = true;
        }
        s.blockedSince = null;
        break;
      }
      case 'SubagentStart':
        s.subagentCount += 1;
        break;
      case 'SubagentStop':
        s.subagentCount = Math.max(0, s.subagentCount - 1);
        break;
      case 'PreCompact':
        if (s.phase !== State.COMPACTING) s.preCompactPhase = s.phase;
        s.phase = State.COMPACTING;
        break;
      case 'PostCompact':
        s.phase = s.preCompactPhase ?? (s.turnStartAt != null ? State.WORKING : State.IDLE);
        s.preCompactPhase = null;
        break;
      case 'SessionEnd':
        this.sessions.delete(ev.session_id);
        if (this.manualPrimaryId === ev.session_id) this.manualPrimaryId = null;
        return;
      default:
        // unknown/unhandled event: keep the session alive, no phase change.
        break;
    }
  }

  private finishTurn(s: Session, now: number): void {
    if (s.turnStartAt != null) {
      s.lastTurnDurationS = Math.max(0, Math.round((now - s.turnStartAt) / 1000));
      s.turnStartAt = null;
    }
  }

  private handleNotification(s: Session, ev: HookEvent, now: number): void {
    const t = ev.notification_type ?? '';
    if (FILE_LIKE_NOTIFICATIONS_BLOCKED.has(t)) {
      s.phase = State.BLOCKED;
      s.blockedSince = s.blockedSince ?? now;
    } else if (WAITING_NOTIFICATIONS.has(t)) {
      s.phase = State.WAITING_INPUT;
    } else if (t === 'quota_auto_resume_fired') {
      if (s.phase === State.LIMITED) {
        s.phase = s.turnStartAt != null ? State.WORKING : State.IDLE;
      }
      s.limitedReason = null;
    }
    // auth_success / elicitation_complete / elicitation_response / agent_completed /
    // quota_auto_resume_stale / quota_auto_resume_disabled: no phase change, just keep-alive.
  }

  /** ACK from the watch: clears unacked DONE / stops BLOCKED haptic repeats upstream. */
  ack(sessionId: string): void {
    const s = this.sessions.get(sessionId);
    if (!s) return;
    s.unackedDone = false;
  }

  select(sessionId: string): void {
    if (this.sessions.has(sessionId)) this.manualPrimaryId = sessionId;
  }

  applyStatusline(sessionId: string, data: {
    repoName?: string | null;
    projectDir?: string | null;
    modelDisplayName?: string | null;
    model?: Model;
    effort?: Effort;
    fastMode?: boolean;
    thinking?: boolean;
    contextPct?: number | null;
    costCents?: number | null;
    linesAdded?: number | null;
    linesRemoved?: number | null;
    cacheWarm?: boolean;
    cacheExpiresAt?: number | null;
    prNumber?: number | null;
    prReview?: PrReview;
  }): void {
    const now = this.now();
    const s = this.getOrCreate(sessionId, now);
    s.lastEventAt = now;
    s.quiet = false;
    if (data.repoName || data.projectDir) s.projectRaw = projectLabel(data.repoName, data.projectDir);
    if (data.modelDisplayName != null) s.modelDisplayName = data.modelDisplayName;
    if (data.model != null) s.model = data.model;
    if (data.effort != null) s.effort = data.effort;
    if (data.fastMode != null) s.fastMode = data.fastMode;
    if (data.thinking != null) s.thinking = data.thinking;
    if (data.contextPct !== undefined) s.contextPct = data.contextPct;
    if (data.costCents !== undefined) {
      s.costCents = data.costCents;
      if (data.costCents != null) {
        const day = this.localDay(now);
        const cur = this.costLedger.get(sessionId);
        if (!cur || cur.day !== day || data.costCents > cur.maxCents) {
          this.costLedger.set(sessionId, { maxCents: Math.max(data.costCents, cur && cur.day === day ? cur.maxCents : 0), day });
        }
      }
    }
    if (data.linesAdded != null) s.linesAdded = data.linesAdded;
    if (data.linesRemoved != null) s.linesRemoved = data.linesRemoved;
    if (data.cacheWarm != null) s.cacheWarm = data.cacheWarm;
    if (data.cacheExpiresAt !== undefined) s.cacheExpiresAt = data.cacheExpiresAt;
    if (data.prNumber !== undefined) s.prNumber = data.prNumber;
    if (data.prReview != null) s.prReview = data.prReview;
  }

  costTodayCents(now = this.now()): number | null {
    const day = this.localDay(now);
    let sum = 0;
    let any = false;
    for (const entry of this.costLedger.values()) {
      if (entry.day === day) {
        sum += entry.maxCents;
        any = true;
      }
    }
    return any ? sum : null;
  }

  /** Reap: flag quiet/hung WORKING sessions, demote idle sessions to STALE, remove long-stale ones. */
  reap(now = this.now()): void {
    for (const [id, s] of this.sessions) {
      const idleMs = now - s.lastEventAt;
      if (s.phase === State.WORKING) {
        s.quiet = idleMs > REAP_STALE_MS;
      } else {
        s.quiet = false;
        if (idleMs > REAP_STALE_MS && s.phase !== State.STALE) {
          s.phase = State.STALE;
        }
      }
      if (s.phase === State.STALE && idleMs > REAP_REMOVE_MS) {
        this.sessions.delete(id);
        if (this.manualPrimaryId === id) this.manualPrimaryId = null;
      }
    }
  }

  list(): Session[] {
    return [...this.sessions.values()];
  }

  get(id: string): Session | undefined {
    return this.sessions.get(id);
  }

  /** Global state across all live sessions, per SPEC priority; `forceAtLeastLimited`
   *  lets the caller fold in a >=100% rate-limit window without a session-level trigger. */
  globalState(forceAtLeastLimited = false): number {
    const sessions = this.list();
    if (sessions.length === 0) {
      return forceAtLeastLimited ? State.LIMITED : State.NONE;
    }
    // A quiet (possibly hung / closed without SessionEnd) WORKING session
    // must not keep the watch saying WORKING: rank it as IDLE.
    const eff = (s: Session) => (s.quiet ? State.IDLE : s.phase);
    let best = sessions[0]!;
    let bestIdx = priorityIndex(eff(best), best.unackedDone);
    for (const s of sessions.slice(1)) {
      const idx = priorityIndex(eff(s), s.unackedDone);
      if (idx < bestIdx) {
        best = s;
        bestIdx = idx;
      }
    }
    if (forceAtLeastLimited) {
      const limitedIdx = PRIORITY.indexOf(State.LIMITED);
      if (bestIdx > limitedIdx) return State.LIMITED;
    }
    const phase = eff(best);
    return phase === State.DONE && !best.unackedDone ? State.IDLE : phase;
  }

  /** Primary session id: watch override (if still valid) wins; else first BLOCKED
   *  (earliest blockedSince); else most recently active. */
  primaryId(): string | null {
    if (this.manualPrimaryId && this.sessions.has(this.manualPrimaryId)) {
      return this.manualPrimaryId;
    }
    const sessions = this.list();
    if (sessions.length === 0) return null;
    const blocked = sessions.filter((s) => s.phase === State.BLOCKED);
    if (blocked.length > 0) {
      blocked.sort((a, b) => (a.blockedSince ?? a.lastEventAt) - (b.blockedSince ?? b.lastEventAt));
      return blocked[0]!.id;
    }
    const live = (s: Session) => !s.quiet && s.phase !== State.STALE;
    sessions.sort((a, b) => Number(live(b)) - Number(live(a)) || b.lastEventAt - a.lastEventAt);
    return sessions[0]!.id;
  }

  /** Transcript says the turn was interrupted (no Stop hook fires for that). */
  markInterrupted(id: string): boolean {
    const s = this.sessions.get(id);
    if (!s || !(s.phase === State.WORKING || s.phase === State.BLOCKED || s.phase === State.COMPACTING)) return false;
    s.phase = State.IDLE;
    s.turnStartAt = null;
    s.blockedSince = null;
    s.quiet = false;
    return true;
  }

  /** Transcript says the Claude process exited (no SessionEnd reached us). */
  remove(id: string): boolean {
    if (this.manualPrimaryId === id) this.manualPrimaryId = null;
    return this.sessions.delete(id);
  }

  /** Up to 4 sessions for the SESSIONS message, most important first. */
  orderedForRows(): Session[] {
    const primary = this.primaryId();
    const sessions = this.list();
    sessions.sort((a, b) => {
      if (a.id === primary) return -1;
      if (b.id === primary) return 1;
      const pa = priorityIndex(a.phase, a.unackedDone);
      const pb = priorityIndex(b.phase, b.unackedDone);
      if (pa !== pb) return pa - pb;
      return b.lastEventAt - a.lastEventAt;
    });
    return sessions.slice(0, 4);
  }

  blockedCount(): number {
    return this.list().filter((s) => s.phase === State.BLOCKED).length;
  }
}
