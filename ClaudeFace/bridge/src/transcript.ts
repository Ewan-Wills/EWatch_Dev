/**
 * Session details for Claude Code sessions that never run the statusline
 * (the desktop app's Code tab doesn't): model + context % from the tail of
 * the session transcript, and a stable project name from the git root.
 *
 * Privacy: only token counts and the model id are read out of the
 * transcript; no message text leaves this module.
 */
import { execFileSync } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';

const TAIL_BYTES = 512 * 1024;
const MIN_READ_INTERVAL_MS = 2000;
const DEFAULT_WINDOW = 200_000;
const EXTENDED_WINDOW = 1_000_000;

export interface TranscriptInfo {
  modelId: string;
  modelDisplayName: string;
  contextTokens: number;
  contextPct: number;
}

/** 'claude-opus-5-5' -> 'Opus 5.5', 'claude-haiku-4-5-20251001' -> 'Haiku 4.5'. */
export function modelDisplayName(id: string): string {
  const m = /claude-(opus|sonnet|haiku|fable)-(\d+)(?:-(\d{1,2}))?(?:-\d{8})?/i.exec(id);
  if (!m) return id.replace(/^claude-/, '');
  const fam = m[1]!.charAt(0).toUpperCase() + m[1]!.slice(1).toLowerCase();
  return m[3] ? `${fam} ${m[2]}.${m[3]}` : `${fam} ${m[2]}`;
}

/** Last assistant message's model + input-side token count (the statusline's formula). */
export function parseTranscriptTail(text: string): TranscriptInfo | null {
  const lines = text.split('\n');
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = lines[i];
    if (!line || !line.includes('"usage"')) continue;
    let d: { type?: string; isSidechain?: boolean; message?: { model?: string; usage?: Record<string, number> } };
    try {
      d = JSON.parse(line);
    } catch {
      continue; // first line of the tail is usually cut mid-record
    }
    const msg = d.message;
    if (d.type !== 'assistant' || d.isSidechain || !msg?.usage || !msg.model || msg.model === '<synthetic>') continue;
    const u = msg.usage;
    const tokens = (u.input_tokens ?? 0) + (u.cache_creation_input_tokens ?? 0) + (u.cache_read_input_tokens ?? 0);
    // The transcript doesn't record the window size: 1M if the model id says
    // so or the context already exceeds 200k, else 200k.
    const window = /\[1m\]/i.test(msg.model) || tokens > DEFAULT_WINDOW ? EXTENDED_WINDOW : DEFAULT_WINDOW;
    return {
      modelId: msg.model,
      modelDisplayName: modelDisplayName(msg.model),
      contextTokens: tokens,
      contextPct: Math.min(100, Math.round((tokens / window) * 100)),
    };
  }
  return null;
}

const lastRead = new Map<string, number>();

export async function readTranscriptInfo(transcriptPath: string, now = Date.now()): Promise<TranscriptInfo | null> {
  if (!path.isAbsolute(transcriptPath) || !transcriptPath.endsWith('.jsonl')) return null;
  const prev = lastRead.get(transcriptPath) ?? 0;
  if (now - prev < MIN_READ_INTERVAL_MS) return null;
  lastRead.set(transcriptPath, now);
  let fh: fs.promises.FileHandle | null = null;
  try {
    fh = await fs.promises.open(transcriptPath, 'r');
    const { size } = await fh.stat();
    const start = Math.max(0, size - TAIL_BYTES);
    const buf = Buffer.alloc(size - start);
    await fh.read(buf, 0, buf.length, start);
    return parseTranscriptTail(buf.toString('utf8'));
  } catch {
    return null;
  } finally {
    await fh?.close();
  }
}

const rootCache = new Map<string, string>();

/** Git top-level for a directory (cached), else the directory itself. */
export function projectRoot(dir: string): string {
  const hit = rootCache.get(dir);
  if (hit) return hit;
  let root = dir;
  try {
    root = execFileSync('git', ['-C', dir, 'rev-parse', '--show-toplevel'], {
      encoding: 'utf8',
      timeout: 1000,
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim() || dir;
  } catch {
    // not a repo / git missing: use the directory
  }
  rootCache.set(dir, root);
  return root;
}

export type TranscriptEnd = 'exited' | 'interrupted' | 'other';

/**
 * How a transcript ends. A final `cost-state` record is written when the
 * Claude process exits (seen on every closed desktop session, never as the
 * last record of a live one); a last user message of
 * "[Request interrupted by user..." means the turn was cancelled, which fires
 * no Stop hook.
 */
export function transcriptEnd(text: string): TranscriptEnd {
  const lines = text.trim().split('\n');
  let first = true;
  for (let i = lines.length - 1; i >= 0; i--) {
    let d: { type?: string; isSidechain?: boolean; message?: { content?: unknown } };
    try {
      d = JSON.parse(lines[i]!);
    } catch {
      continue;
    }
    if (first && d.type === 'cost-state') return 'exited';
    first = false;
    if (d.isSidechain) continue;
    if (d.type === 'assistant') return 'other';
    if (d.type === 'user') {
      const c = d.message?.content;
      const text = typeof c === 'string' ? c : Array.isArray(c) ? c.map((x: { text?: string }) => x?.text ?? '').join('') : '';
      if (text.startsWith('[Request interrupted by user')) return 'interrupted';
      return 'other';
    }
  }
  return 'other';
}

export async function readTranscriptEnd(transcriptPath: string): Promise<{ end: TranscriptEnd; mtimeMs: number } | null> {
  if (!path.isAbsolute(transcriptPath) || !transcriptPath.endsWith('.jsonl')) return null;
  let fh: fs.promises.FileHandle | null = null;
  try {
    fh = await fs.promises.open(transcriptPath, 'r');
    const { size, mtimeMs } = await fh.stat();
    const start = Math.max(0, size - 64 * 1024);
    const buf = Buffer.alloc(size - start);
    await fh.read(buf, 0, buf.length, start);
    return { end: transcriptEnd(buf.toString('utf8')), mtimeMs };
  } catch {
    return null;
  } finally {
    await fh?.close();
  }
}
