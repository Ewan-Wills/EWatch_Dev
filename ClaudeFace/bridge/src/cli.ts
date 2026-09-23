#!/usr/bin/env node
import { loadConfig, ensureConfigDir } from './config';
import { OAuthUsagePoller } from './oauthUsage';
import { RateLimitTracker } from './ratelimits';
import { Bridge } from './bridge';
import { createServer } from './server';
import { FakeLink, SerialLink } from './link';
import { runSimulate } from './simulate';
import { StatuspagePoller } from './statuspage';

function parseArgs(argv: string[]): { flags: Record<string, string | boolean>; positional: string[] } {
  const flags: Record<string, string | boolean> = {};
  const positional: string[] = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!;
    if (a.startsWith('--')) {
      const key = a.slice(2);
      const next = argv[i + 1];
      if (next !== undefined && !next.startsWith('--')) {
        flags[key] = next;
        i++;
      } else {
        flags[key] = true;
      }
    } else {
      positional.push(a);
    }
  }
  return { flags, positional };
}

async function main(): Promise<void> {
  const argv = process.argv.slice(2);
  const { flags, positional } = parseArgs(argv);
  const cmd = positional[0] ?? 'serve';

  if (cmd === 'simulate') {
    const cfg = loadConfig();
    await runSimulate({
      port: typeof flags.port === 'string' ? flags.port : null,
      speed: flags.speed ? Number(flags.speed) : 1,
      dryRun: Boolean(flags['dry-run']),
      watchLogFile: cfg.watchLogFile,
    });
    return;
  }

  // default: serve
  const cfg = loadConfig();
  ensureConfigDir(cfg);
  const rl = RateLimitTracker.loadFromDisk(cfg.ratelimitsFile);

  const fakeLinkPath = flags['fake-link'];
  const link = fakeLinkPath
    ? new FakeLink(undefined, (line) => {
        if (typeof fakeLinkPath === 'string') {
          require('fs').appendFileSync(fakeLinkPath, `${line}`);
        } else {
          process.stdout.write(line);
        }
      })
    : new SerialLink(cfg.serialPath, cfg.watchLogFile);

  const bridge = new Bridge({ cfg, link, rl, trace: Boolean(flags.trace) });
  bridge.start();

  if (link instanceof SerialLink) {
    await link.start();
  } else {
    link.connectNow();
  }

  if (cfg.oauthUsage.enabled) {
    new OAuthUsagePoller((u) => bridge.onUsage(u)).start();
  }

  if (cfg.statuspage.enabled) {
    new StatuspagePoller(bridge).start();
  }

  const server = createServer(bridge);
  server.listen(cfg.port, '127.0.0.1', () => {
    // eslint-disable-next-line no-console
    console.log(`[claude-bridge] listening on 127.0.0.1:${cfg.port}`);
  });

  const shutdown = () => {
    bridge.stop();
    link.close();
    server.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

main().catch((err) => {
  // eslint-disable-next-line no-console
  console.error('[claude-bridge] fatal:', err);
  process.exit(1);
});
