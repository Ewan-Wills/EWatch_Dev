import {
  frame,
  encodeState,
  encodeSessions,
  encodeTime,
  decodeState,
  decodeSessions,
  unframe,
  StateMsg,
  SessionsMsg,
  SessionRowMsg,
  PROTO_VERSION,
  State,
  Model,
  Effort,
  Flags,
  PFlags,
  Sentinel,
} from './protocol.gen';
import { FakeLink, SerialLink } from './link';
import { utcOffsetMinutes } from './util';

export interface SimulateOpts {
  port: string | null;
  speed: number; // multiplier; 20 = 20x real time
  dryRun: boolean;
  watchLogFile: string;
}

function baseState(now: number, overrides: Partial<StateMsg> = {}): StateMsg {
  return {
    version: PROTO_VERSION,
    seq: 0,
    flags: Flags.BRIDGE_OK,
    globalState: State.NONE,
    fiveUsed: Sentinel.PCT_UNKNOWN,
    fiveResetsAt: 0,
    sevenUsed: Sentinel.PCT_UNKNOWN,
    sevenResetsAt: 0,
    fivePace: Sentinel.PACE_UNKNOWN,
    sevenPace: Sentinel.PACE_UNKNOWN,
    projectedHit: 0,
    rlAgeS: 65535,
    fiveElapsed: Sentinel.PCT_UNKNOWN,
    sevenElapsed: Sentinel.PCT_UNKNOWN,
    sessionCount: 0,
    blockedCount: 0,
    subagentCount: 0,
    primaryIndex: 255,
    pState: State.NONE,
    pCtx: Sentinel.PCT_UNKNOWN,
    pModel: Model.UNKNOWN,
    pEffort: Effort.UNKNOWN,
    pFlags: 0,
    pTurnElapsedS: 0,
    pLastTurnS: 0,
    pToolCount: 0,
    pCurrentTool: '',
    pProject: '',
    pModelName: '',
    pCacheExpiresAt: 0,
    pLinesAdded: 0,
    pLinesRemoved: 0,
    pCostCents: Sentinel.U16_UNKNOWN,
    costTodayCents: Sentinel.U16_UNKNOWN,
    pPrNumber: 0,
    pPrReview: 0,
    bridgeNow: Math.floor(now / 1000),
    ...overrides,
  };
}

function row(overrides: Partial<SessionRowMsg> = {}): SessionRowMsg {
  return { state: State.NONE, ctx: Sentinel.PCT_UNKNOWN, project: '', turnElapsedS: 0, ...overrides };
}

function baseSessions(rows: SessionRowMsg[]): SessionsMsg {
  return { version: PROTO_VERSION, seq: 0, count: rows.length, rows: rows.slice(0, 4) };
}

interface Ctx {
  send(state: Partial<StateMsg>, sessions: SessionRowMsg[]): void;
  sleep(ms: number): Promise<void>;
  now(): number;
}

interface Scene {
  name: string;
  run(ctx: Ctx): Promise<void>;
}

const SCENE_S = 6;

function scenes(): Scene[] {
  const list: Scene[] = [];

  list.push({
    name: 'NONE',
    async run(ctx) {
      ctx.send({ globalState: State.NONE, sessionCount: 0 }, []);
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'IDLE',
    async run(ctx) {
      ctx.send(
        { globalState: State.IDLE, sessionCount: 1, primaryIndex: 0, pState: State.IDLE, pProject: 'ewatch', pCtx: 5 },
        [row({ state: State.IDLE, ctx: 5, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'WORKING',
    async run(ctx) {
      const tools = ['Bash|npm test', 'Edit|face.ts', 'Read|SPEC.md', 'Write|state.ts', 'Bash|git status', 'Grep|TODO'];
      for (let i = 0; i < SCENE_S; i++) {
        ctx.send(
          {
            globalState: State.WORKING,
            sessionCount: 1,
            primaryIndex: 0,
            pState: State.WORKING,
            pProject: 'ewatch',
            pCurrentTool: tools[i % tools.length],
            pToolCount: i + 1,
            pTurnElapsedS: i,
            pCtx: 30,
          },
          [row({ state: State.WORKING, ctx: 30, project: 'ewatch', turnElapsedS: i })],
        );
        await ctx.sleep(1000);
      }
    },
  });

  list.push({
    name: 'COMPACTING',
    async run(ctx) {
      ctx.send(
        { globalState: State.COMPACTING, sessionCount: 1, primaryIndex: 0, pState: State.COMPACTING, pProject: 'ewatch' },
        [row({ state: State.COMPACTING, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'DONE_short_turn',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.DONE,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.DONE,
          pProject: 'ewatch',
          pLastTurnS: 12,
          pFlags: PFlags.UNACKED_DONE,
        },
        [row({ state: State.DONE, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'DONE_long_turn_90s_buzz',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.DONE,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.DONE,
          pProject: 'ewatch',
          pLastTurnS: 90,
          pFlags: PFlags.UNACKED_DONE,
        },
        [row({ state: State.DONE, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'WAITING_INPUT',
    async run(ctx) {
      ctx.send(
        { globalState: State.WAITING_INPUT, sessionCount: 1, primaryIndex: 0, pState: State.WAITING_INPUT, pProject: 'ewatch' },
        [row({ state: State.WAITING_INPUT, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'BLOCKED',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.BLOCKED,
          sessionCount: 1,
          blockedCount: 1,
          primaryIndex: 0,
          pState: State.BLOCKED,
          pProject: 'ewatch',
          pCurrentTool: 'Bash|rm -rf build',
        },
        [row({ state: State.BLOCKED, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'LIMITED_5h_100pct',
    async run(ctx) {
      const resetsAt = Math.floor(ctx.now() / 1000) + 47 * 60;
      ctx.send(
        {
          globalState: State.LIMITED,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.LIMITED,
          pProject: 'ewatch',
          fiveUsed: 100,
          fiveResetsAt: resetsAt,
          fiveElapsed: 92,
          fivePace: 8,
          rlAgeS: 30,
        },
        [row({ state: State.LIMITED, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'NO_RATE_LIMITS',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.IDLE,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.IDLE,
          pProject: 'ewatch',
          flags: Flags.BRIDGE_OK | Flags.NO_RATE_LIMITS,
          fiveUsed: 255,
          sevenUsed: 255,
          fiveElapsed: 255,
          sevenElapsed: 255,
          rlAgeS: 65535,
        },
        [row({ state: State.IDLE, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'RL_STALE',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.IDLE,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.IDLE,
          pProject: 'ewatch',
          flags: Flags.BRIDGE_OK | Flags.RL_STALE,
          fiveUsed: 40,
          fiveResetsAt: Math.floor(ctx.now() / 1000) + 3600,
          rlAgeS: 20 * 60,
        },
        [row({ state: State.IDLE, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'pace_ahead_70_40',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.WORKING,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.WORKING,
          pProject: 'ewatch',
          fiveUsed: 70,
          fiveElapsed: 40,
          fivePace: 30,
          fiveResetsAt: Math.floor(ctx.now() / 1000) + 9000,
        },
        [row({ state: State.WORKING, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'pace_behind_20_60',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.WORKING,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.WORKING,
          pProject: 'ewatch',
          fiveUsed: 20,
          fiveElapsed: 60,
          fivePace: -40,
          fiveResetsAt: Math.floor(ctx.now() / 1000) + 7200,
        },
        [row({ state: State.WORKING, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'projected_hit_set',
    async run(ctx) {
      const resetsAt = Math.floor(ctx.now() / 1000) + 3 * 3600;
      const hitAt = Math.floor(ctx.now() / 1000) + 1800;
      ctx.send(
        {
          globalState: State.WORKING,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.WORKING,
          pProject: 'ewatch',
          fiveUsed: 85,
          fiveElapsed: 50,
          fivePace: 35,
          fiveResetsAt: resetsAt,
          projectedHit: hitAt,
        },
        [row({ state: State.WORKING, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  for (const pct of [80, 95]) {
    list.push({
      name: `5h_crossing_${pct}pct`,
      async run(ctx) {
        ctx.send(
          {
            globalState: State.WORKING,
            sessionCount: 1,
            primaryIndex: 0,
            pState: State.WORKING,
            pProject: 'ewatch',
            fiveUsed: pct,
            fiveElapsed: pct - 5,
            fivePace: 5,
            fiveResetsAt: Math.floor(ctx.now() / 1000) + 3600,
          },
          [row({ state: State.WORKING, project: 'ewatch' })],
        );
        await ctx.sleep(SCENE_S * 1000);
      },
    });
  }

  list.push({
    name: '4_sessions_6_total',
    async run(ctx) {
      const rows = [
        row({ state: State.WORKING, project: 'ewatch', turnElapsedS: 30, ctx: 25 }),
        row({ state: State.BLOCKED, project: 'agx', ctx: 10 }),
        row({ state: State.IDLE, project: 'aurora', ctx: 5 }),
        row({ state: State.DONE, project: 'site', ctx: 60 }),
      ];
      ctx.send(
        {
          globalState: State.BLOCKED,
          sessionCount: 6,
          blockedCount: 1,
          primaryIndex: 1,
          pState: State.BLOCKED,
          pProject: 'agx',
        },
        rows,
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  for (const n of [0, 1, 2, 3, 5]) {
    list.push({
      name: `subagents_${n}`,
      async run(ctx) {
        ctx.send(
          {
            globalState: State.WORKING,
            sessionCount: 1,
            primaryIndex: 0,
            pState: State.WORKING,
            pProject: 'ewatch',
            subagentCount: n,
          },
          [row({ state: State.WORKING, project: 'ewatch' })],
        );
        await ctx.sleep(SCENE_S * 1000);
      },
    });
  }

  for (const pct of [65, 75, 92]) {
    list.push({
      name: `context_${pct}pct`,
      async run(ctx) {
        ctx.send(
          {
            globalState: State.WORKING,
            sessionCount: 1,
            primaryIndex: 0,
            pState: State.WORKING,
            pProject: 'ewatch',
            pCtx: pct,
          },
          [row({ state: State.WORKING, project: 'ewatch', ctx: pct })],
        );
        await ctx.sleep(SCENE_S * 1000);
      },
    });
  }

  list.push({
    name: 'cache_cold_in_3m',
    async run(ctx) {
      ctx.send(
        {
          globalState: State.DONE,
          sessionCount: 1,
          primaryIndex: 0,
          pState: State.DONE,
          pProject: 'ewatch',
          pFlags: PFlags.UNACKED_DONE | PFlags.CACHE_WARM,
          pCacheExpiresAt: Math.floor(ctx.now() / 1000) + 180,
        },
        [row({ state: State.DONE, project: 'ewatch' })],
      );
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'protocol_version_mismatch',
    async run(ctx) {
      ctx.send({ version: 99, globalState: State.WORKING, sessionCount: 1, primaryIndex: 0, pState: State.WORKING, pProject: 'ewatch' }, [
        row({ state: State.WORKING, project: 'ewatch' }),
      ]);
      await ctx.sleep(SCENE_S * 1000);
    },
  });

  list.push({
    name: 'bridge_disconnect_100s',
    async run(ctx) {
      // Send nothing for 100s (scaled by --speed) so the watch's own 90s OFFLINE timeout fires.
      await ctx.sleep(100 * 1000);
      ctx.send({ globalState: State.IDLE, sessionCount: 1, primaryIndex: 0, pState: State.IDLE, pProject: 'ewatch' }, [
        row({ state: State.IDLE, project: 'ewatch' }),
      ]);
      await ctx.sleep(1000);
    },
  });

  return list;
}

export async function runSimulate(opts: SimulateOpts): Promise<void> {
  const link = opts.dryRun
    ? new FakeLink(undefined, (line) => printDecoded(line))
    : new SerialLink(opts.port, opts.watchLogFile);

  if (link instanceof SerialLink) {
    // Listen before start(): start() can connect (and emit) before it resolves.
    const connected = new Promise<void>((resolve) => link.once('connect', () => resolve()));
    await link.start();
    await connected;
  } else {
    link.connectNow();
  }

  let stateSeq = 0;
  let sessionsSeq = 0;
  const timeMsg = { version: PROTO_VERSION, epoch: Math.floor(Date.now() / 1000), utcOffsetMin: utcOffsetMinutes() };
  link.send(frame('T', encodeTime(timeMsg)));

  const ctx: Ctx = {
    now: () => Date.now(),
    sleep: (ms: number) => new Promise((resolve) => setTimeout(resolve, Math.max(0, ms / opts.speed))),
    send: (state, sessionRows) => {
      const sessions = baseSessions(sessionRows);
      sessions.seq = ++sessionsSeq;
      link.send(frame('L', encodeSessions(sessions)));
      const st = baseState(Date.now(), state);
      st.seq = ++stateSeq;
      link.send(frame('S', encodeState(st)));
    },
  };

  for (const scene of scenes()) {
    // eslint-disable-next-line no-console
    console.log(`=== scene: ${scene.name} ===`);
    await scene.run(ctx);
  }

  link.close();
}

function printDecoded(line: string): void {
  const trimmed = line.trim();
  if (trimmed.startsWith('#CFS')) {
    // eslint-disable-next-line no-console
    console.log('STATE', JSON.stringify(decodeState(unpack(trimmed))));
  } else if (trimmed.startsWith('#CFL')) {
    // eslint-disable-next-line no-console
    console.log('SESSIONS', JSON.stringify(decodeSessions(unpack(trimmed))));
  } else {
    // eslint-disable-next-line no-console
    console.log(trimmed);
  }
}

function unpack(line: string): Buffer {
  const decoded = unframe(line);
  if (!decoded) throw new Error(`bad frame: ${line}`);
  return decoded.payload;
}
