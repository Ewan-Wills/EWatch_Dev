import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';

export interface OAuthUsageConfig {
  enabled: boolean;
}

export interface StatuspageConfig {
  enabled: boolean;
}

export interface BridgeConfig {
  port: number;
  serialPath: string | null; // explicit override; null = auto-probe
  configDir: string;
  ratelimitsFile: string;
  watchLogFile: string;
  statuspage: StatuspageConfig;
  oauthUsage: OAuthUsageConfig;
  quietHours: { startMin: number; endMin: number }; // minutes since local midnight
}

const DEFAULT_CONFIG_DIR = path.join(os.homedir(), '.config', 'claude-bridge');
const DEFAULT_LOG_DIR = path.join(os.homedir(), 'Library', 'Logs');

function parseHHMM(s: string, fallback: number): number {
  const m = /^(\d{1,2}):(\d{2})$/.exec(s ?? '');
  if (!m) return fallback;
  const h = Number(m[1]);
  const mi = Number(m[2]);
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return fallback;
  return h * 60 + mi;
}

/**
 * Loads config from (in increasing precedence): built-in defaults,
 * ~/.config/claude-bridge/config.json (or $CLAUDE_BRIDGE_CONFIG_DIR/config.json),
 * then environment variables (CLAUDE_BRIDGE_PORT, CLAUDE_BRIDGE_SERIAL_PATH).
 */
export function loadConfig(env: NodeJS.ProcessEnv = process.env): BridgeConfig {
  const configDir = env.CLAUDE_BRIDGE_CONFIG_DIR
    ? path.resolve(env.CLAUDE_BRIDGE_CONFIG_DIR)
    : DEFAULT_CONFIG_DIR;
  const configFile = path.join(configDir, 'config.json');

  let fileCfg: Record<string, unknown> = {};
  try {
    fileCfg = JSON.parse(fs.readFileSync(configFile, 'utf8'));
  } catch {
    // absent or unparsable config file: use defaults
  }

  const f = fileCfg as {
    port?: number;
    serialPath?: string;
    statuspage?: { enabled?: boolean };
    oauthUsage?: { enabled?: boolean };
    quietHours?: { start?: string; end?: string };
  };

  const port = Number(env.CLAUDE_BRIDGE_PORT ?? f.port ?? 7337);
  const serialPath = env.CLAUDE_BRIDGE_SERIAL_PATH ?? f.serialPath ?? null;

  return {
    port: Number.isFinite(port) ? port : 7337,
    serialPath,
    configDir,
    ratelimitsFile: path.join(configDir, 'ratelimits.json'),
    watchLogFile: path.join(DEFAULT_LOG_DIR, 'claude-bridge-watch.log'),
    statuspage: { enabled: Boolean(f.statuspage?.enabled) },
    oauthUsage: { enabled: Boolean(f.oauthUsage?.enabled) },
    quietHours: {
      startMin: parseHHMM(f.quietHours?.start ?? '23:00', 23 * 60),
      endMin: parseHHMM(f.quietHours?.end ?? '08:00', 8 * 60),
    },
  };
}

export function ensureConfigDir(cfg: BridgeConfig): void {
  fs.mkdirSync(cfg.configDir, { recursive: true });
}
