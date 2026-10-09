import { Model, Effort, PrReview } from './protocol.gen';

export interface StatuslinePayload {
  session_id?: string;
  rate_limits?: {
    five_hour?: { used_percentage?: number; resets_at?: number };
    seven_day?: { used_percentage?: number; resets_at?: number };
  };
  context_window?: { used_percentage?: number | null; context_window_size?: number };
  model?: { display_name?: string; id?: string };
  effort?: { level?: string };
  fast_mode?: boolean;
  thinking?: { enabled?: boolean };
  cost?: { total_cost_usd?: number; total_lines_added?: number; total_lines_removed?: number };
  workspace?: { project_dir?: string; repo?: { name?: string } };
  prompt_cache?: { warm?: boolean; expires_at?: number | null };
  pr?: { number?: number; review_state?: string };
  [k: string]: unknown;
}

export function mapModel(displayName: string | undefined | null): Model {
  if (!displayName) return Model.UNKNOWN;
  const d = displayName.toLowerCase();
  if (d.includes('opus')) return Model.OPUS;
  if (d.includes('sonnet')) return Model.SONNET;
  if (d.includes('haiku')) return Model.HAIKU;
  if (d.includes('fable')) return Model.FABLE;
  return Model.OTHER;
}

export function mapEffort(level: string | undefined | null): Effort {
  switch (level) {
    case 'low':
      return Effort.LOW;
    case 'medium':
      return Effort.MEDIUM;
    case 'high':
      return Effort.HIGH;
    case 'xhigh':
      return Effort.XHIGH;
    case 'max':
      return Effort.MAX;
    default:
      return Effort.UNKNOWN;
  }
}

export function mapPrReview(state: string | undefined | null, hasPr: boolean): PrReview {
  if (!hasPr) return PrReview.NONE;
  switch (state) {
    case 'approved':
      return PrReview.APPROVED;
    case 'pending':
      return PrReview.PENDING;
    case 'changes_requested':
      return PrReview.CHANGES_REQUESTED;
    case 'draft':
      return PrReview.DRAFT;
    default:
      return PrReview.OTHER;
  }
}

export interface ParsedStatusline {
  sessionId: string | null;
  rateLimits: {
    fiveHour?: { usedPercentage?: number | null; resetsAt?: number | null };
    sevenDay?: { usedPercentage?: number | null; resetsAt?: number | null };
  };
  session: {
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
  };
}

/** Parses the raw statusline JSON (privacy-safe: never reads prompt text or file contents,
 *  none of which the statusline schema carries anyway). */
export function parseStatusline(payload: StatuslinePayload): ParsedStatusline {
  const hasPr = payload.pr?.number != null;
  return {
    sessionId: typeof payload.session_id === 'string' ? payload.session_id : null,
    rateLimits: {
      fiveHour: payload.rate_limits?.five_hour
        ? {
            usedPercentage: payload.rate_limits.five_hour.used_percentage ?? null,
            resetsAt: payload.rate_limits.five_hour.resets_at ?? null,
          }
        : undefined,
      sevenDay: payload.rate_limits?.seven_day
        ? {
            usedPercentage: payload.rate_limits.seven_day.used_percentage ?? null,
            resetsAt: payload.rate_limits.seven_day.resets_at ?? null,
          }
        : undefined,
    },
    session: {
      repoName: payload.workspace?.repo?.name ?? null,
      projectDir: payload.workspace?.project_dir ?? null,
      modelDisplayName: payload.model?.display_name ?? null,
      model: mapModel(payload.model?.display_name),
      effort: mapEffort(payload.effort?.level),
      fastMode: Boolean(payload.fast_mode),
      thinking: Boolean(payload.thinking?.enabled),
      contextPct:
        payload.context_window?.used_percentage != null
          ? Math.round(payload.context_window.used_percentage)
          : null,
      costCents:
        payload.cost?.total_cost_usd != null ? Math.round(payload.cost.total_cost_usd * 100) : null,
      linesAdded: payload.cost?.total_lines_added ?? null,
      linesRemoved: payload.cost?.total_lines_removed ?? null,
      cacheWarm: Boolean(payload.prompt_cache?.warm),
      cacheExpiresAt: payload.prompt_cache?.expires_at ?? null,
      prNumber: payload.pr?.number ?? null,
      prReview: mapPrReview(payload.pr?.review_state, hasPr),
    },
  };
}
